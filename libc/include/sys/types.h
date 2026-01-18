// libc/include/sys/types.h
#pragma once

#include <stdint.h>
#include <stddef.h>

// Process and thread types
typedef int pid_t;
typedef int tid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;

// File system types
typedef long off_t;
typedef unsigned long ino_t;
typedef unsigned int mode_t;
typedef unsigned long nlink_t;
typedef long blksize_t;
typedef long blkcnt_t;
typedef long time_t;
typedef long dev_t;

// Size types
typedef long ssize_t;

// Clock types
typedef long clock_t;
typedef long clockid_t;

// File descriptor set (for select/poll)
typedef struct {
    unsigned long fds_bits[16];
} fd_set;
