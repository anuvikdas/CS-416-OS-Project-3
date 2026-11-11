# CS 416 Operating Systems Project 3

Build a user-level virtual memory library with a two-level page table and a direct-mapped translation lookaside buffer (TLB).

**Due November 15, 2026 at 11:59 p.m.** · [Assignment instructions](Project3/Project3.pdf) · [Starter code](Project3/code)

Update each row's **Status** as work moves from **Not started** to **In progress** to **Completed**. Add your initials under **Owner** when you pick up a task.

## Part 1 · Virtual memory system (70 points)

| Task | Done when | Owner | Status |
| --- | --- | --- | --- |
| Address layout | Use a 32-bit virtual address space with one top-level directory and two page-table levels. Derive the offset width from `log2(PGSIZE)` and split the remaining bits across the levels. A 4 KiB page uses 10 directory bits, 10 table bits, and 12 offset bits. Other power-of-two sizes can split the index bits unevenly. | | Not started |
| `set_physical_mem()` | Allocate contiguous simulated RAM of size `MEMSIZE` with `mmap()` or `malloc()`. Store the page directory and page tables in that buffer and account for the physical pages they occupy. Keep `MEMSIZE` (1 GiB by default) separate from `MAX_MEMSIZE` (4 GiB of virtual address space). Use a type wide enough for values at or above 2³² in the 32-bit build. | | Not started |
| Bitmaps and `get_next_avail()` | Track physical pages with one bit per page and find the next available page or virtual range for an allocation. Track virtual availability with a one-bit-per-page bitmap or by walking the page tables, as allowed on PDF page 2. | | Not started |
| `translate()` | Walk both page-table levels, reject unmapped addresses, and apply the original page offset to get the corresponding physical address. | | Not started |
| `page_map()` | Walk the page directory, create a second-level table when needed, and install a mapping for a previously unmapped virtual page. | | Not started |
| `t_malloc()` | Round each request up to whole pages and return the start of a contiguous virtual range. Map each page to available physical memory, allowing noncontiguous physical pages. Two separate 1-byte requests each use a page. At 4 KiB, a 4,097-byte request uses two pages. | | Not started |
| `t_free()` | Validate that the entire requested range is allocated before freeing it. Clear page-table entries and release the corresponding physical bits and virtual tracking, including when physical pages are noncontiguous. At 4 KiB, freeing 5,000 bytes from `0x1000` releases two pages. The starter API returns `void`. FAQ 7 permits freeing the containing page or printing an error for a mid-page pointer and says that case is untested. | | Not started |
| `put_value()` | Validate the library virtual address range and copy bytes to translated physical pages, including writes that cross page boundaries. Return `0` on success and `-1` on failure. | | Not started |
| `get_value()` | Validate the library virtual address range and copy bytes from translated physical pages into the caller's buffer, including reads that cross page boundaries. Use the same translation path when the TLB is enabled. | | Not started |
| Invalid accesses | Review the error behavior for accesses to unallocated memory. FAQ 8 calls for printing an error and exiting without raising a segfault. The `put_value()` section specifically gives `-1` as its failure return. | | Not started |
| `mat_mult()` | Read both input matrices and write the result through the VM library. Use the row-major byte offset `(i * size + j) * sizeof(element)` and verify the result against the sample benchmark. | | Not started |
| API and thread safety | Preserve the starter signatures for `t_malloc()`, `t_free()`, `put_value()`, `get_value()`, and `mat_mult()`. Synchronize shared memory-management state for concurrent callers. Document any permitted helper extensions in the report. | | Not started |

## Part 2 · Direct-mapped TLB (20 points)

Start this part after the page-table and allocation code works and the Part 1 matrix benchmark output is recorded.

| Task | Done when | Owner | Status |
| --- | --- | --- | --- |
| Initialize the TLB | Initialize empty entries with the page table. Store a valid bit or equivalent, a virtual page number, and a physical page number. Use `TLB_ENTRIES` from `my_vm.h` and support different entry counts. | | Not started |
| Look up translations | Check the TLB before walking the page table. Verify the virtual page number in the indexed entry. On a hit, apply the offset and skip the walk. On a miss, walk the table and cache the translation if found. | | Not started |
| Insert and evict | Add translations after mapping new pages and after successful page-table lookups on misses. Replace the existing entry at the same index when entries conflict. The PDF calls these helpers `check_TLB()` and `add_TLB()`. Keep the signatures declared in the starter header for `check_in_tlb()` and `put_in_tlb()`. | | Not started |
| Keep translations current | Invalidate cached translations for freed virtual pages so a stale hit cannot access reclaimed physical memory. Protect TLB entries and statistics under concurrent calls. | | Not started |
| Report the miss rate | Track hits and misses, account for compulsory misses as described in FAQ 14, and implement `print_TLB_missrate()` using the definition from class. Save the observed rate for the report. | | Not started |

## Build and verify

Use the provided Makefiles with `-m32`. Build the library before the benchmark. The PDF lists compatible Rutgers iLab hosts, including `ilab1.cs.rutgers.edu` through `ilab4.cs.rutgers.edu`. A missing `gnu/stubs-32.h` points to a missing 32-bit build dependency on the development host.

| Task | Done when | Owner | Status |
| --- | --- | --- | --- |
| Build | From the repository root, run `make -C Project3/code` and then `make -C Project3/code/benchmark`. | | Not started |
| Verify Part 1 | Run `./test` from `Project3/code/benchmark` and save the output before adding the TLB. Exercise single-page and multi-page allocation, reads and writes across boundaries, noncontiguous physical backing, and invalid frees. | | Not started |
| Verify Part 2 | Run the benchmark again and record the miss rate. Check TLB hits, misses, index collisions, invalidation after a free, and changed `TLB_ENTRIES` values. | | Not started |
| Test memory configurations | Rebuild and test with different supported power-of-two page sizes in multiples of 4 KiB and with different simulated physical memory sizes. Record the configurations and outcomes. | | Not started |
| Test concurrency | Run `./mtest` from `Project3/code/benchmark`. Extend the sample or vary the thread count as needed. FAQ 5 says up to 50 threads is sufficient. | | Not started |

## Report (10 points)

| Task | Done when | Owner | Status |
| --- | --- | --- | --- |
| Explain the implementation | Describe the logic of each virtual memory function and document any helper extensions. | | Not started |
| Include benchmark results | Include the Part 1 benchmark output and the observed Part 2 TLB miss rate. | | Not started |
| Document page-size support | Explain how the implementation supports different page sizes in multiples of 4 KiB. | | Not started |
| List known issues | Describe any remaining issues or limitations. | | Not started |
| Prepare the report PDF | Include the project partner's name and NetID, if applicable, and export the report as a PDF. | | Not started |

## Canvas submission

| Task | Done when | Owner | Status |
| --- | --- | --- | --- |
| Confirm archive format | Check Canvas or confirm with course staff. The opening line of PDF §6 says to submit files without compressing them, while item 1 says to ZIP the code, Makefile, and benchmark code. | | Not started |
| Submit code and build files | Include all code, the Makefile, benchmark code, and any support sources or Makefiles you created or modified. Upload them in the confirmed format. | | Not started |
| Attach the report and verify submission | Attach the report PDF separately as directed, then verify all required files appear in Canvas before the deadline. | | Not started |
