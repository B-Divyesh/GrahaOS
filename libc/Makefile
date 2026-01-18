# libc/Makefile
# Phase 7c: Build system for GrahaOS C library

PREFIX ?= /home/atman/GrahaOS/toolchain
ARCH ?= x86_64-elf

CC = $(PREFIX)/bin/$(ARCH)-gcc
AR = $(PREFIX)/bin/$(ARCH)-ar
# FIXED: Use -mcmodel=small for user-space (not kernel)
# User-space code runs at low addresses (0x400000+)
# DEBUG: Using -O0 to diagnose printf %p issue
CFLAGS = -Wall -Wextra -O0 -g -nostdlib -ffreestanding -fno-stack-protector \
         -mno-red-zone -mcmodel=small -fno-pie -fno-pic -m64 \
         -march=x86-64 -Iinclude

# Source files
SRC_DIR = src
OBJ_DIR = obj

SOURCES = $(wildcard $(SRC_DIR)/*.c)
OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Output library
LIBNAME = libc.a

.PHONY: all clean

all: $(OBJ_DIR) $(LIBNAME)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(LIBNAME): $(OBJECTS)
	@echo "Creating static library $@"
	$(AR) rcs $@ $^
	@echo "libc.a built successfully!"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(LIBNAME)

install:
	@echo "libc.a is ready at $(shell pwd)/$(LIBNAME)"
