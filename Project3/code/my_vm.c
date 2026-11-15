#include "my_vm.h"
#include <stdint.h>
#include <string.h>
#include <pthread.h>

/* Library addresses are 32-bit numbers, never directly dereferenced pointers.
 * Bitmap bit 1 means occupied. Virtual page zero is reserved for NULL.
 * Page-table entries encode (physical frame number << 1) | valid.
 */
#define VM_ADDRESS_BITS 32U
#define VM_FIRST_VIRTUAL_PAGE 1U
#define VM_ENTRY_VALID 1UL

struct vm_layout
{
    unsigned int offset_bits;
    unsigned int table_bits;
    unsigned int directory_bits;
    uint32_t offset_mask;
    uint32_t table_mask;
    uint64_t directory_entries;
    uint64_t table_entries;
    uint64_t virtual_pages;
    uint64_t physical_frames;
    size_t virtual_bitmap_bytes;
    size_t physical_bitmap_bytes;
    size_t directory_frames;
    size_t table_frames;
};

/* The directory and all
 * second-level tables must occupy frames inside physical_memory.
 * The bitmaps themselves may be allocated outside simulated RAM.
 */
static struct vm_layout layout;
static unsigned char *physical_memory;
static unsigned char *virtual_bitmap;
static unsigned char *physical_bitmap;
static pde_t *page_directory;
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;

/* Quotient/remainder rounding avoids overflow from adding PGSIZE - 1. */
static uint64_t pages_for_bytes(uint64_t bytes)
{
    return bytes / PGSIZE + (bytes % PGSIZE != 0);
}

static size_t bitmap_bytes(uint64_t pages)
{
    return (size_t)(pages / 8 + (pages % 8 != 0));
}

/* Calculate locally so an invalid configuration leaves the output untouched.
 * Return 0 on success, -1 for an unsupported configuration.
 */
static int calculate_layout(struct vm_layout *result)
{
    struct vm_layout next = {0};
    uint64_t page_size = PGSIZE;
    uint64_t remaining;

    if (page_size < 4096 || page_size >= (UINT64_C(1) << VM_ADDRESS_BITS) ||
        (page_size & (page_size - 1)) != 0 ||
        (uint64_t)MEMSIZE < page_size)
        return -1;

    for (remaining = page_size; remaining > 1; remaining >>= 1)
        next.offset_bits++;

    next.table_bits = (VM_ADDRESS_BITS - next.offset_bits) / 2;
    next.directory_bits = VM_ADDRESS_BITS - next.offset_bits - next.table_bits;
    next.offset_mask = (uint32_t)(page_size - 1);
    next.directory_entries = UINT64_C(1) << next.directory_bits;
    next.table_entries = UINT64_C(1) << next.table_bits;
    next.table_mask = (uint32_t)(next.table_entries - 1);
    next.virtual_pages = (uint64_t)MAX_MEMSIZE / page_size;
    next.physical_frames = (uint64_t)MEMSIZE / page_size;
    next.virtual_bitmap_bytes = bitmap_bytes(next.virtual_pages);
    next.physical_bitmap_bytes = bitmap_bytes(next.physical_frames);
    next.directory_frames = (size_t)pages_for_bytes(
        next.directory_entries * sizeof(pde_t));
    next.table_frames = (size_t)pages_for_bytes(
        next.table_entries * sizeof(pte_t));
    *result = next;
    return 0;
}

/* Callers validate bitmap indexes against the corresponding page count. */
static bool bitmap_test(const unsigned char *bitmap, uint64_t index)
{
    return (bitmap[index / 8] & (1U << (index % 8))) != 0;
}

static void bitmap_set(unsigned char *bitmap, uint64_t index)
{
    bitmap[index / 8] |= (unsigned char)(1U << (index % 8));
}

static void bitmap_clear(unsigned char *bitmap, uint64_t index)
{
    bitmap[index / 8] &= (unsigned char)~(1U << (index % 8));
}

static uint32_t virtual_page_number(uint32_t address)
{
    return address >> layout.offset_bits;
}

static uint32_t directory_index(uint32_t address)
{
    return address >> (layout.offset_bits + layout.table_bits);
}

static uint32_t table_index(uint32_t address)
{
    return virtual_page_number(address) & layout.table_mask;
}

static uint32_t page_offset(uint32_t address)
{
    return address & layout.offset_mask;
}

static pte_t entry_for_frame(uint32_t frame)
{
    return ((pte_t)frame << 1) | VM_ENTRY_VALID;
}

static bool entry_is_valid(pte_t entry)
{
    return (entry & VM_ENTRY_VALID) != 0;
}

static uint32_t entry_frame(pte_t entry)
{
    return (uint32_t)(entry >> 1);
}

/* Internal helpers require vm_lock. Tables need consecutive physical frames
 * when their entry arrays span more than one frame.
 */
static int64_t reserve_frames(size_t count)
{
    uint64_t run = 0;
    for (uint64_t frame = 0; frame < layout.physical_frames; frame++) {
        run = bitmap_test(physical_bitmap, frame) ? 0 : run + 1;
        if (run == count) {
            uint64_t first = frame + 1 - count;
            for (uint64_t i = first; i <= frame; i++)
                bitmap_set(physical_bitmap, i);
            memset(physical_memory + first * PGSIZE, 0, count * (size_t)PGSIZE);
            return (int64_t)first;
        }
    }
    return -1;
}

static int initialize_memory_locked(void)
{
    if (physical_memory != NULL)
        return 0;
    if (calculate_layout(&layout) != 0 ||
        layout.directory_frames > layout.physical_frames)
        return -1;

    unsigned char *ram = malloc((size_t)MEMSIZE);
    unsigned char *vbits = calloc(layout.virtual_bitmap_bytes, 1);
    unsigned char *pbits = calloc(layout.physical_bitmap_bytes, 1);
    if (ram == NULL || vbits == NULL || pbits == NULL) {
        free(ram);
        free(vbits);
        free(pbits);
        return -1;
    }
    physical_memory = ram;
    virtual_bitmap = vbits;
    physical_bitmap = pbits;
    /* All frames are free here, so the root occupies frames starting at zero. */
    reserve_frames(layout.directory_frames);
    page_directory = (pde_t *)physical_memory;
    bitmap_set(virtual_bitmap, 0);
    return 0;
}

static pte_t *translate_locked(pde_t *pgdir, void *va)
{
    uintptr_t address = (uintptr_t)va;
    if (physical_memory == NULL || pgdir != page_directory ||
        address >= (uint64_t)MAX_MEMSIZE || address < PGSIZE)
        return NULL;

    pde_t directory_entry = pgdir[directory_index((uint32_t)address)];
    if (!entry_is_valid(directory_entry))
        return NULL;
    uint32_t table_frame = entry_frame(directory_entry);
    if ((uint64_t)table_frame + layout.table_frames > layout.physical_frames)
        return NULL;
    pte_t *table = (pte_t *)(physical_memory + (size_t)table_frame * PGSIZE);
    pte_t entry = table[table_index((uint32_t)address)];
    if (!entry_is_valid(entry) || entry_frame(entry) >= layout.physical_frames)
        return NULL;
    return (pte_t *)(physical_memory + (size_t)entry_frame(entry) * PGSIZE +
                    page_offset((uint32_t)address));
}

/* Mapping owns the supplied free frame on success; it never replaces a map.
 * Reserving the data frame before the table prevents table/data overlap.
 */
static int page_map_locked(pde_t *pgdir, void *va, void *pa)
{
    uintptr_t address = (uintptr_t)va;
    uintptr_t physical = (uintptr_t)pa;
    uintptr_t base = (uintptr_t)physical_memory;
    if (physical_memory == NULL || pgdir != page_directory ||
        address < PGSIZE || address >= (uint64_t)MAX_MEMSIZE ||
        page_offset((uint32_t)address) != 0 || physical < base ||
        (uint64_t)(physical - base) >= (uint64_t)MEMSIZE ||
        (physical - base) % PGSIZE != 0)
        return -1;

    uint32_t frame = (uint32_t)((physical - base) / PGSIZE);
    if (frame >= layout.physical_frames || bitmap_test(physical_bitmap, frame))
        return -1;
    pde_t *directory_entry = &pgdir[directory_index((uint32_t)address)];
    bitmap_set(physical_bitmap, frame);
    if (!entry_is_valid(*directory_entry)) {
        int64_t table_frame = reserve_frames(layout.table_frames);
        if (table_frame < 0) {
            bitmap_clear(physical_bitmap, frame);
            return -1;
        }
        *directory_entry = entry_for_frame((uint32_t)table_frame);
    }
    pte_t *table = (pte_t *)(physical_memory +
                            (size_t)entry_frame(*directory_entry) * PGSIZE);
    pte_t *entry = &table[table_index((uint32_t)address)];
    if (entry_is_valid(*entry)) {
        bitmap_clear(physical_bitmap, frame);
        return -1;
    }
    *entry = entry_for_frame(frame);
    bitmap_set(virtual_bitmap, virtual_page_number((uint32_t)address));
    return 0;
}

/*
Function responsible for allocating and setting your physical memory
*/
void set_physical_mem()
{
    pthread_mutex_lock(&vm_lock);
    if (initialize_memory_locked() != 0)
        fprintf(stderr, "Unable to initialize simulated physical memory\n");
    pthread_mutex_unlock(&vm_lock);
}

/*
 * Part 2: Add a virtual to physical page translation to the TLB.
 * Feel free to extend the function arguments or return type.
 */
int add_TLB(void *va, void *pa)
{
    (void)va;
    (void)pa;

    /*Part 2 HINT: Add a virtual to physical page translation to the TLB */

    return -1;
}

/*
 * Part 2: Check TLB for a valid translation.
 * Returns the physical page address.
 * Feel free to extend this function and change the return type.
 */
pte_t *
check_TLB(void *va)
{

    (void)va;

    return NULL;
}

/*
 * Part 2: Print TLB miss rate.
 * Feel free to extend the function arguments or return type.
 */
void print_TLB_missrate()
{
    double miss_rate = 0;

    /*Part 2 Code here to calculate and print the TLB miss rate*/

    fprintf(stderr, "TLB miss rate %lf \n", miss_rate);
}

/*
The function takes a virtual address and page directories starting address and
performs translation to return the physical address
*/
pte_t *translate(pde_t *pgdir, void *va)
{
    pthread_mutex_lock(&vm_lock);
    pte_t *result = translate_locked(pgdir, va);
    pthread_mutex_unlock(&vm_lock);
    return result;
}

/*
The function takes a page directory address, virtual address, physical address
as an argument, and sets a page table entry. This function will walk the page
directory to see if there is an existing mapping for a virtual address. If the
virtual address is not present, then a new entry will be added
*/
int page_map(pde_t *pgdir, void *va, void *pa)
{
    pthread_mutex_lock(&vm_lock);
    int result = page_map_locked(pgdir, va, pa);
    pthread_mutex_unlock(&vm_lock);
    return result;
}

/* Find a contiguous run of virtual pages while holding vm_lock. */
static uintptr_t next_virtual_range(uint64_t count)
{
    uint64_t run = 0;
    for (uint64_t page = VM_FIRST_VIRTUAL_PAGE; page < layout.virtual_pages; page++) {
        run = bitmap_test(virtual_bitmap, page) ? 0 : run + 1;
        if (run == count)
            return (uintptr_t)((page + 1 - count) * PGSIZE);
    }
    return 0;
}

void *get_next_avail(int num_pages)
{
    if (num_pages <= 0)
        return NULL;
    pthread_mutex_lock(&vm_lock);
    uintptr_t address = 0;
    if (initialize_memory_locked() == 0)
        address = next_virtual_range((uint64_t)num_pages);
    pthread_mutex_unlock(&vm_lock);
    return (void *)address;
}

static void unmap_page(uint32_t address)
{
    pde_t *directory_entry = &page_directory[directory_index(address)];
    uint32_t table_frame = entry_frame(*directory_entry);
    pte_t *table = (pte_t *)(physical_memory + (size_t)table_frame * PGSIZE);
    pte_t *entry = &table[table_index(address)];

    bitmap_clear(physical_bitmap, entry_frame(*entry));
    bitmap_clear(virtual_bitmap, virtual_page_number(address));
    *entry = 0;
    for (uint64_t index = 0; index < layout.table_entries; index++) {
        if (entry_is_valid(table[index]))
            return;
    }
    *directory_entry = 0;
    for (size_t index = 0; index < layout.table_frames; index++)
        bitmap_clear(physical_bitmap, table_frame + index);
}

void *t_malloc(unsigned int num_bytes)
{
    if (num_bytes == 0)
        return NULL;

    uint64_t count = pages_for_bytes(num_bytes);
    uintptr_t first = 0;
    pthread_mutex_lock(&vm_lock);
    if (initialize_memory_locked() == 0 && count < layout.physical_frames)
        first = next_virtual_range(count);
    if (first != 0) {
        uint64_t mapped = 0;
        uint64_t frame = 0;
        while (mapped < count) {
            while (frame < layout.physical_frames && bitmap_test(physical_bitmap, frame))
                frame++;
            if (frame == layout.physical_frames)
                break;
            uintptr_t address = (uintptr_t)(first + mapped * PGSIZE);
            unsigned char *page = physical_memory + frame * PGSIZE;
            if (page_map_locked(page_directory, (void *)address, page) != 0)
                break;
            memset(page, 0, PGSIZE);
            mapped++;
            frame++;
        }
        if (mapped != count) {
            while (mapped > 0) {
                mapped--;
                unmap_page((uint32_t)(first + mapped * PGSIZE));
            }
            first = 0;
        }
    }
    pthread_mutex_unlock(&vm_lock);
    return (void *)first;
}

/* Validate the full byte range before freeing or copying any pages. */
static bool range_is_mapped(uintptr_t address, uint64_t size)
{
    if (physical_memory == NULL || address < PGSIZE ||
        address >= (uint64_t)MAX_MEMSIZE ||
        size > (uint64_t)MAX_MEMSIZE - address || size == 0)
        return false;
    uint32_t first = virtual_page_number((uint32_t)address);
    uint32_t last = virtual_page_number((uint32_t)(address + size - 1));
    for (uint32_t page = first; page <= last; page++) {
        if (!bitmap_test(virtual_bitmap, page))
            return false;
    }
    return true;
}

void t_free(void *va, int size)
{
    uintptr_t address = (uintptr_t)va;
    pthread_mutex_lock(&vm_lock);
    if (size <= 0 || address % PGSIZE != 0 ||
        !range_is_mapped(address, (uint64_t)size)) {
        fprintf(stderr, "Cannot free an invalid virtual memory range\n");
    } else {
        uint64_t end = (uint64_t)address + size;
        for (uint64_t page = address; page < end; page += PGSIZE)
            unmap_page((uint32_t)page);
    }
    pthread_mutex_unlock(&vm_lock);
}

static int copy_value(void *va, void *val, int size, bool write)
{
    if (size == 0)
        return 0;

    uintptr_t address = (uintptr_t)va;
    unsigned char *buffer = val;
    pthread_mutex_lock(&vm_lock);
    if (size < 0 || val == NULL || !range_is_mapped(address, (uint64_t)size)) {
        pthread_mutex_unlock(&vm_lock);
        fprintf(stderr, "Cannot access an invalid virtual memory range\n");
        return -1;
    }

    size_t remaining = (size_t)size;
    while (remaining > 0) {
        size_t chunk = PGSIZE - page_offset((uint32_t)address);
        if (chunk > remaining)
            chunk = remaining;
        void *physical = translate_locked(page_directory, (void *)address);
        if (write)
            memcpy(physical, buffer, chunk);
        else
            memcpy(buffer, physical, chunk);
        address += chunk;
        buffer += chunk;
        remaining -= chunk;
    }
    pthread_mutex_unlock(&vm_lock);
    return 0;
}

int put_value(void *va, void *val, int size)
{
    return copy_value(va, val, size, true);
}

void get_value(void *va, void *val, int size)
{
    if (copy_value(va, val, size, false) != 0)
        exit(EXIT_FAILURE);
}

void mat_mult(void *mat1, void *mat2, int size, void *answer)
{
    if (size <= 0)
        return;
    uint64_t matrix_bytes = (uint64_t)size * size * sizeof(unsigned int);
    uint64_t limit = (uint64_t)MAX_MEMSIZE;
    uintptr_t left = (uintptr_t)mat1;
    uintptr_t right = (uintptr_t)mat2;
    uintptr_t result = (uintptr_t)answer;
    if (left >= limit || right >= limit || result >= limit ||
        matrix_bytes > limit - left || matrix_bytes > limit - right ||
        matrix_bytes > limit - result) {
        fprintf(stderr, "Matrix exceeds the virtual address space\n");
        return;
    }

    for (int row = 0; row < size; row++) {
        for (int column = 0; column < size; column++) {
            unsigned int sum = 0;
            for (int index = 0; index < size; index++) {
                uintptr_t left_offset = ((uint64_t)row * size + index) * sizeof(sum);
                uintptr_t right_offset = ((uint64_t)index * size + column) * sizeof(sum);
                unsigned int a, b;
                get_value((void *)(left + left_offset), &a, sizeof(a));
                get_value((void *)(right + right_offset), &b, sizeof(b));
                sum += a * b;
            }
            uintptr_t offset = ((uint64_t)row * size + column) * sizeof(sum);
            if (put_value((void *)(result + offset), &sum, sizeof(sum)) != 0)
                return;
        }
    }
}
