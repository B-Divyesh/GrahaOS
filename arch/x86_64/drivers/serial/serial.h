// arch/x86_64/drivers/serial/serial.h
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>

void serial_init(void);
void serial_register_cap(void);  // Phase 14: deferred CAN registration
void serial_putc(char c);
void serial_write(const char *str);
// Phase 15b: atomic bounded write (holds g_serial_tx_lock for the whole
// burst). Use this when multiple writers may contend — SYS_WRITE console
// path and klog mirror both do.
void serial_write_n(const char *buf, size_t len);
void serial_write_hex(uint64_t value);
void serial_write_dec(uint64_t value);

#endif
