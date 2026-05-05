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

// Phase 27 closeout: serial RX path. ISR pushes incoming bytes into a
// small ring; SYS_GETC drains both the keyboard buffer AND this ring so
// users can type either in the QEMU window (PS/2) or in the host terminal
// (`-serial stdio` UART).
void serial_irq_handler(void);
char serial_pop_char(void);   // 0 if empty (non-blocking)

#endif
