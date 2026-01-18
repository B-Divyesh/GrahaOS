// libc/include/unistd.h
#pragma once

#include <stddef.h>
#include <stdint.h>

// Standard file descriptors
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// SEEK constants
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// System calls
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
int open(const char *pathname, int flags, ...);

// Process control
int getpid(void);
int fork(void);
int exec(const char *path);
void _exit(int status) __attribute__((noreturn));

// Program break
int brk(void *addr);
void *sbrk(intptr_t increment);

// Working directory
char *getcwd(char *buf, size_t size);
int chdir(const char *path);

// Sleep
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
