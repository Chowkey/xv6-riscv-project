// kernel/getproc.h
#ifndef GETPROC_H
#define GETPROC_H

#include "types.h"

// Structure to hold process information
struct procinfo {
  int pid;                // Process ID
  int state;              // Process state (use int instead of enum)
  uint64 sz;              // Size of process memory (bytes)
  char name[16];          // Process name
};

#endif