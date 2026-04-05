#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pid;
  uint64 addr1, addr2;

  printf("Testing dynamic multi-region mmap...\n");

  // Map first shared memory region with key 1
  addr1 = mmap(1);
  if(addr1 == 0){
    printf("mmap key 1 failed\n");
    exit(1);
  }
  printf("Mapped region 1 at 0x%p\n", (void*)addr1);

  // Map second shared memory region with key 2
  addr2 = mmap(2);
  if(addr2 == 0){
    printf("mmap key 2 failed\n");
    exit(1);
  }
  printf("Mapped region 2 at 0x%p\n", (void*)addr2);

  if (addr1 == addr2) {
    printf("Error: both regions mapped to the same address!\n");
    exit(1);
  }

  // Write to shared memory
  int *data1 = (int*)addr1;
  int *data2 = (int*)addr2;
  *data1 = 111;
  *data2 = 222;
  printf("Parent wrote: region1=%d, region2=%d\n", *data1, *data2);

  // Fork a child process
  pid = fork();
  if(pid < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(pid == 0){
    // Child process must explicitly map the regions using the same keys
    uint64 c_addr1 = mmap(1);
    uint64 c_addr2 = mmap(2);

    if(c_addr1 == 0 || c_addr2 == 0) {
      printf("Child mmap failed\n");
      exit(1);
    }
    
    int *c_data1 = (int*)c_addr1;
    int *c_data2 = (int*)c_addr2;

    printf("Child read: region1=%d, region2=%d\n", *c_data1, *c_data2);

    // Modify the shared data
    *c_data1 = 333;
    *c_data2 = 444;
    printf("Child wrote: region1=%d, region2=%d\n", *c_data1, *c_data2);

    // Test unmapping in child
    if(munmap(c_addr1) < 0 || munmap(c_addr2) < 0)
      printf("Child: munmap failed\n");
    else
      printf("Child: munmap succeeded\n");

    exit(0);
  } else {
    // Parent process
    wait(0);

    // Verify the child's modification is visible
    printf("Parent read after child: region1=%d, region2=%d\n", *data1, *data2);

    if (*data1 != 333 || *data2 != 444) {
      printf("Error: Parent did not see child's modifications!\n");
    } else {
      printf("Test passed!\n");
    }

    // Unmap shared memory
    if(munmap(addr1) < 0 || munmap(addr2) < 0)
      printf("Parent: munmap failed\n");
    else
      printf("Parent: munmap succeeded\n");
  }

  exit(0);
}

