// arch/x86_64/cpu/syscall/syscall.h
#pragma once
#include <stdint.h>
#include "../interrupts.h"
#include "../smp.h"
// Define system call numbers
#define SYS_TEST 0
#define SYS_PUTC 1001

// Filesystem Syscalls
#define SYS_OPEN  1002
#define SYS_READ  1003
#define SYS_CLOSE 1004

// GCP Syscall for Phase 6b
#define SYS_GCP_EXECUTE 1005

// NEW Syscalls for Phase 6c
#define SYS_GETC 1006
#define SYS_EXEC 1007

#define SYS_DEBUG 9999

#define SYS_EXIT 1008

#define SYS_WAIT 1009
#define SYS_WRITE  1010
#define SYS_CREATE 1011
#define SYS_MKDIR  1012
#define SYS_STAT   1013
#define SYS_READDIR 1014
#define SYS_SYNC 1015
#define SYS_BRK  1016

// Phase 7d: Modern Process Management
#define SYS_SPAWN  1017
#define SYS_KILL   1018
#define SYS_SIGNAL 1019
#define SYS_GETPID 1020

// Phase 8a: System State Introspection
#define SYS_GET_SYSTEM_STATE 1021

// Phase 8b: Capability Activation Network
#define SYS_CAP_ACTIVATE   1031
#define SYS_CAP_DEACTIVATE 1032
#define SYS_CAP_REGISTER   1033
#define SYS_CAP_UNREGISTER 1034

// Phase 8c: AI Metadata in GrahaFS
#define SYS_SET_AI_METADATA 1035
#define SYS_GET_AI_METADATA 1036
#define SYS_SEARCH_BY_TAG   1037

// Phase 8d: CAN Event Propagation
#define SYS_CAP_WATCH   1038
#define SYS_CAP_UNWATCH 1039
#define SYS_CAP_POLL    1040

// Phase 9a: Network
#define SYS_NET_IFCONFIG 1041

// Phase 9b: Network Status
#define SYS_NET_STATUS 1042

void syscall_init(void);
void syscall_dispatcher(struct syscall_frame *frame);

