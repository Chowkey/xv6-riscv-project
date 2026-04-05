#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void
test_multiple_regions()
{
  printf("Testing multiple shared memory regions...\n");

  uint64 addr1 = mmap(1);
  uint64 addr2 = mmap(2);

  if(addr1 == 0 || addr2 == 0){
    printf("mmap failed\n");
    exit(1);
  }

  if(addr1 == addr2){
    printf("Error: Multiple regions mapped to the same address 0x%p\n", (void*)addr1);
    exit(1);
  }

  printf("Region 1 mapped at 0x%p\n", (void*)addr1);
  printf("Region 2 mapped at 0x%p\n", (void*)addr2);

  int *data1 = (int*)addr1;
  int *data2 = (int*)addr2;

  *data1 = 111;
  *data2 = 222;

  if(*data1 != 111 || *data2 != 222){
    printf("Error: Data corruption in multiple regions\n");
    exit(1);
  }

  printf("Multiple regions test passed\n");

  munmap(addr1);
  munmap(addr2);
}

void
test_sharing()
{
  printf("Testing sharing between processes...\n");

  int key = 42;
  uint64 addr = mmap(key);
  if(addr == 0){
    printf("mmap failed\n");
    exit(1);
  }

  int *data = (int*)addr;
  *data = 0;

  int pid = fork();
  if(pid < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(pid == 0){
    // Child
    uint64 child_addr = mmap(key);
    if(child_addr == 0){
        printf("Child mmap failed\n");
        exit(1);
    }
    int *child_data = (int*)child_addr;
    *child_data = 1234;
    munmap(child_addr);
    exit(0);
  } else {
    // Parent
    wait(0);
    if(*data == 1234){
      printf("Sharing test passed: Parent saw child's write\n");
    } else {
      printf("Sharing test failed: Parent saw %d, expected 1234\n", *data);
      exit(1);
    }
    munmap(addr);
  }
}

int
main(int argc, char *argv[])
{
  test_multiple_regions();
  test_sharing();
  printf("All SHM tests passed!\n");
  exit(0);
}
