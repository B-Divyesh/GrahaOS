// libc/include/stddef.h
#pragma once

// Standard type definitions
typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef long ssize_t;

// NULL pointer
#ifndef NULL
#define NULL ((void *)0)
#endif

// Offset of member in structure
#define offsetof(type, member) __builtin_offsetof(type, member)
