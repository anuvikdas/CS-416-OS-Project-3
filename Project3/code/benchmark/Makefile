
CC = gcc
CFLAGS = -m32

all: test mtest
test: test.c ../my_vm.h ../libmy_vm.a
	$(CC) $(CFLAGS) test.c -L../ -lmy_vm -o test -lpthread

mtest: multi_test.c ../my_vm.h ../libmy_vm.a
	$(CC) $(CFLAGS) multi_test.c -L../ -lmy_vm -o mtest -lpthread

clean:
	rm -rf test mtest
