// arch/x86_64/drivers/serial/serial.c
// Serial port driver for logging and debugging

#include "serial.h"
#include "../../cpu/ports.h"
#include "../../../../kernel/cap/can.h"
#include "../../../../kernel/sync/spinlock.h"

#define PORT_COM1 0x3F8

static int serial_initialized = 0;

// Phase 15b fix: a single TX lock serialises byte bursts from concurrent
// writers (user SYS_WRITE, klog mirror, panic path). Without it, a user
// process's "ok 1\n" and a child process's printf output interleave
// character-by-character on the UART, corrupting TAP parser input and
// producing flaky test runs (~10% failure rate pre-fix).
//
// The lock is acquired at the granularity of *complete byte bursts* — one
// acquire per serial_write or per mirror_entry_to_serial call — so the
// latency impact is negligible for the TAP-parse-friendly outcome of
// "each writer's bytes land contiguously".
static spinlock_t g_serial_tx_lock;
static int serial_tx_lock_initialised = 0;

static void ensure_tx_lock(void) {
    if (!serial_tx_lock_initialised) {
        spinlock_init(&g_serial_tx_lock, "serial_tx");
        serial_tx_lock_initialised = 1;
    }
}

// Phase 27 closeout: serial RX ring. Wraps a 256-byte SPSC ring;
// producer is the COM1 IRQ handler (one writer), consumer is SYS_GETC
// running in syscall context (one reader at a time across CPUs, gated
// by spinlock). Drop-on-overflow if the consumer falls behind — bytes
// from the host terminal aren't worth blocking the ISR over.
#define SERIAL_RX_RING_SIZE 256
static volatile char     s_rx_ring[SERIAL_RX_RING_SIZE];
static volatile uint32_t s_rx_head = 0;   // ISR writes here
static volatile uint32_t s_rx_tail = 0;   // SYS_GETC reads here
static volatile uint64_t s_rx_dropped = 0;

// Driver framework stats callback
static int serial_get_driver_stats(state_driver_stat_t *stats, int max) {
    if (!stats || max < 2) return 0;
    const char *k0 = "initialized";
    for (int i = 0; k0[i] && i < STATE_STAT_KEY_LEN - 1; i++) stats[0].key[i] = k0[i];
    stats[0].key[STATE_STAT_KEY_LEN - 1] = '\0';
    stats[0].value = (uint64_t)serial_initialized;
    const char *k1 = "baud_rate";
    for (int i = 0; k1[i] && i < STATE_STAT_KEY_LEN - 1; i++) stats[1].key[i] = k1[i];
    stats[1].key[STATE_STAT_KEY_LEN - 1] = '\0';
    stats[1].value = 38400;
    return 2;
}

void serial_init(void) {
    ensure_tx_lock();
    // Disable interrupts
    outb(PORT_COM1 + 1, 0x00);

    // Enable DLAB (set baud rate divisor)
    outb(PORT_COM1 + 3, 0x80);

    // Set divisor to 3 (38400 baud)
    outb(PORT_COM1 + 0, 0x03);
    outb(PORT_COM1 + 1, 0x00);

    // 8 bits, no parity, one stop bit
    outb(PORT_COM1 + 3, 0x03);

    // Enable FIFO, clear, with 14-byte threshold
    outb(PORT_COM1 + 2, 0xC7);

    // Enable IRQs, RTS/DSR set, OUT2 high so PIC/IOAPIC sees the line.
    outb(PORT_COM1 + 4, 0x0B);

    // Phase 27 closeout: enable Received-Data-Available interrupt (IER
    // bit 0). Without this the host terminal under `-serial stdio` could
    // never deliver keystrokes — the byte sat in RBR forever and nothing
    // polled it. Now the COM1 IRQ4 line fires on each byte and our ISR
    // (interrupts.c case 36) drains it into s_rx_ring.
    outb(PORT_COM1 + 1, 0x01);

    serial_initialized = 1;

    // Phase 14: CAN registration moved to serial_register_cap() because
    // can_entry_t now lives in a slab that isn't ready at serial_init
    // time (serial init runs before pmm/slab).
}

void serial_register_cap(void) {
    static int serial_cap_registered = 0;
    if (serial_cap_registered) return;
    cap_op_t serial_ops[2];
    cap_op_set(&serial_ops[0], "write", 1, 1);
    cap_op_set(&serial_ops[1], "putc",  1, 1);
    cap_register("serial_output", CAP_DRIVER, CAP_SUBTYPE_SERIAL, -1, NULL, 0,
                 NULL, NULL, serial_ops, 2, serial_get_driver_stats);
    serial_cap_registered = 1;
}

static int is_transmit_empty(void) {
    return inb(PORT_COM1 + 5) & 0x20;
}

// Emit a single byte without taking the TX lock. Caller is responsible for
// holding g_serial_tx_lock if byte-burst serialisation is needed.
static void serial_putc_unlocked(char c) {
    while (!is_transmit_empty());
    outb(PORT_COM1, c);
}

void serial_putc(char c) {
    if (!serial_initialized) return;
    ensure_tx_lock();
    spinlock_acquire(&g_serial_tx_lock);
    serial_putc_unlocked(c);
    spinlock_release(&g_serial_tx_lock);
}

void serial_write(const char *str) {
    if (!serial_initialized) return;
    ensure_tx_lock();
    spinlock_acquire(&g_serial_tx_lock);
    while (*str) {
        serial_putc_unlocked(*str++);
    }
    spinlock_release(&g_serial_tx_lock);
}

// Phase 15b: write a bounded byte range atomically (no interleaving with
// other writers). SYS_WRITE console path + klog serial mirror use this.
void serial_write_n(const char *buf, size_t len) {
    if (!serial_initialized || !buf || len == 0) return;
    ensure_tx_lock();
    spinlock_acquire(&g_serial_tx_lock);
    for (size_t i = 0; i < len; i++) {
        serial_putc_unlocked(buf[i]);
    }
    spinlock_release(&g_serial_tx_lock);
}

void serial_write_hex(uint64_t value) {
    if (!serial_initialized) return;

    serial_write("0x");

    char hex[17];
    hex[16] = '\0';

    for (int i = 15; i >= 0; i--) {
        int digit = value & 0xF;
        hex[i] = digit < 10 ? '0' + digit : 'A' + (digit - 10);
        value >>= 4;
    }

    serial_write(hex);
}

void serial_write_dec(uint64_t value) {
    if (!serial_initialized) return;

    if (value == 0) {
        serial_putc('0');
        return;
    }

    char buf[21];
    int i = 20;
    buf[i] = '\0';

    while (value > 0 && i > 0) {
        buf[--i] = '0' + (value % 10);
        value /= 10;
    }

    serial_write(&buf[i]);
}

// Phase 27 closeout: serial RX ISR. Called from interrupts.c case 36
// (IRQ4 = legacy COM1). Drains all available bytes from the UART RBR
// while the LSR Data-Ready bit is set, pushing each into the ring.
// Most QEMU implementations deliver bytes one at a time but Real
// Hardware (and FIFOs deeper than 1) can present multiple — drain the
// whole batch per IRQ.
void serial_irq_handler(void) {
    // Read IIR (port +2) low 4 bits to identify cause:
    //   0x04 = Received Data Available
    //   0x0C = Character Timeout (FIFO partial drain)
    // Anything else (TX-empty, modem-status, line-status) is benign here.
    while (inb(PORT_COM1 + 5) & 0x01u) {  // LSR.Data-Ready
        char c = (char)inb(PORT_COM1);    // RBR — clears the flag
        // Map CR (0x0D) → LF (0x0A): most terminals send CR on Enter
        // but gash/libtap line-readers split on LF. Without this the
        // user types a command and Enter does nothing.
        if (c == '\r') c = '\n';
        uint32_t next = (s_rx_head + 1) % SERIAL_RX_RING_SIZE;
        if (next == s_rx_tail) {
            // Ring full — drop oldest to keep the ISR forward-progressing.
            s_rx_tail = (s_rx_tail + 1) % SERIAL_RX_RING_SIZE;
            s_rx_dropped++;
        }
        s_rx_ring[s_rx_head] = c;
        s_rx_head = next;
    }
}

char serial_pop_char(void) {
    if (s_rx_head == s_rx_tail) return 0;
    char c = s_rx_ring[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1) % SERIAL_RX_RING_SIZE;
    return c;
}
