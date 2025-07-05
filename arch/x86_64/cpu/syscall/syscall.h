// arch/x86_64/cpu/syscall/syscall.h
#pragma once
#include <stdint.h>
#include "../interrupts.h"

// Define system call numbers
#define SYS_TEST 0
#define SYS_PUTC 1001

// Filesystem Syscalls
#define SYS_OPEN  1002
#define SYS_READ  1003
#define SYS_CLOSE 1004

// NEW GCP Syscall for Phase 6b
#define SYS_GCP_EXECUTE 1005

#define SYS_DEBUG 9999

void syscall_init(void);
void syscall_dispatcher(struct syscall_frame *frame);