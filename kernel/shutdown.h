// kernel/shutdown.h
// Phase 12: orderly kernel shutdown.
//
// kernel_shutdown() attempts ACPI soft-off via a sequence of known
// hypervisor magic ports (QEMU / older QEMU-Bochs / VMware) and falls
// back to a forced triple fault with a zero IDT if all of them fail.
// Under `qemu -no-reboot` a triple fault terminates the emulator
// cleanly — so `make test` gets a well-defined exit either way.

#ifndef GRAHAOS_SHUTDOWN_H
#define GRAHAOS_SHUTDOWN_H

void kernel_shutdown(void) __attribute__((noreturn));

#endif
