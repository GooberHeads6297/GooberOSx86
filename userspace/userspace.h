#ifndef USERSPACE_H
#define USERSPACE_H

#include <stddef.h>
#include <stdint.h>
#include "syscall.h"

void userspace_init(void);

/* Load and run a .gob from the FAT/memfs path (e.g. "Apps/Welcome.gob"). */
int gob_exec(const char* path);

/* In-OS GooberC compiler: compile .gc source to .gob bytecode. */
int gooberc_compile(const char* src_path, const char* out_path);

/* Syscall C entry from asm stub. */
uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3);

void userspace_on_timer(void);

#endif
