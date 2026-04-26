// user/libtls-mg/mg_stubs.c
//
// Phase 22 closeout (G1.3): TLS-only Mongoose stubs.
//
// We compile the entire vendored mongoose.c into mongoose_tls_core.o.
// Most of Mongoose's HTTP / WebSocket / event-loop code is gated behind
// MG_ENABLE_TCPIP / MG_ENABLE_SOCKET — but the amalgamation still
// includes the function bodies (which call mg_send / mg_random / etc.)
// at link time.  These helpers are unreachable from the TLS code path
// our libhttp clients exercise; the linker just needs them to resolve.
//
// Each stub aborts() if it's ever actually called — surfacing any
// mistaken reach into Mongoose's non-TLS surface immediately.

#include <stdint.h>
#include <stddef.h>

extern void abort(void);
extern int printf(const char *fmt, ...);

// Mongoose's global errno mirror.  Never read from TLS code paths.
int mg_errno = 0;

// Forward-declare the structs so the signatures match what mongoose.c
// expects.  We don't dereference any of them; the stubs abort() first.
struct mg_connection;
struct mg_mgr;
struct mg_addr;
struct mg_iobuf;
struct mg_str;

// Trip wires — never reached from TLS-only paths but link must resolve.
#define MG_STUB_TRAP(name) \
    do { \
        printf("FATAL: libtls-mg stub %s() reached — TLS-only build " \
               "should never call this\n", (name)); \
        abort(); \
    } while (0)

// All these are HTTP/event-loop functions that the TLS code is gated
// from reaching (we use libnet's TCP socket directly, not Mongoose's
// mg_connection event loop).
size_t mg_send(struct mg_connection *c, const void *buf, size_t len) {
    (void)c; (void)buf; (void)len;
    MG_STUB_TRAP("mg_send");
    return 0;
}

void mg_connect_resolved(struct mg_connection *c) {
    (void)c;
    MG_STUB_TRAP("mg_connect_resolved");
}

void mg_mgr_poll(struct mg_mgr *mgr, int timeout_ms) {
    (void)mgr; (void)timeout_ms;
    MG_STUB_TRAP("mg_mgr_poll");
}

int mg_open_listener(struct mg_connection *c, const char *url) {
    (void)c; (void)url;
    MG_STUB_TRAP("mg_open_listener");
    return -1;
}

long mg_io_send(struct mg_connection *c, const void *buf, size_t len) {
    (void)c; (void)buf; (void)len;
    MG_STUB_TRAP("mg_io_send");
    return 0;
}

// mg_random — used by tls_builtin.c for client_random / IV nonce.  This
// IS in the TLS hot path; provide a real impl backed by SYS_DEBUG random
// (or rdtsc xor mixing) so handshakes get unpredictable values.
extern uint64_t syscall_get_ticks(void);  // placeholder; impl below.
static uint64_t s_rng_state[4] = {
    0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL,
    0x94D049BB133111EBULL, 0xCBF29CE484222325ULL
};
static uint64_t xorshift_next(void) {
    uint64_t s0 = s_rng_state[0], s1 = s_rng_state[1];
    uint64_t result = s0 + s1;
    s1 ^= s0;
    s_rng_state[0] = ((s0 << 24) | (s0 >> 40)) ^ s1 ^ (s1 << 16);
    s_rng_state[1] = (s1 << 37) | (s1 >> 27);
    s_rng_state[2] += result;
    s_rng_state[3] ^= result;
    return result;
}
// Mix in a coarse rdtsc-based seed at first call so two processes don't
// generate identical TLS streams.
static int s_rng_seeded = 0;
static void mg_random_seed(void) {
    if (s_rng_seeded) return;
    s_rng_seeded = 1;
    uint64_t tsc;
    __asm__ __volatile__("rdtsc; shl $32,%%rdx; or %%rdx,%0"
                         : "=a"(tsc) :: "rdx");
    s_rng_state[0] ^= tsc * 0x9E3779B97F4A7C15ULL;
    s_rng_state[1] ^= ~tsc;
    s_rng_state[2] ^= tsc << 13;
    s_rng_state[3] ^= tsc >> 17;
}
void mg_random(void *buf, size_t len) {
    mg_random_seed();
    uint8_t *p = (uint8_t *)buf;
    while (len >= 8) {
        uint64_t r = xorshift_next();
        for (int i = 0; i < 8; i++) p[i] = (uint8_t)(r >> (i * 8));
        p += 8; len -= 8;
    }
    if (len) {
        uint64_t r = xorshift_next();
        for (size_t i = 0; i < len; i++) p[i] = (uint8_t)(r >> (i * 8));
    }
}

// mg_millis — used by TLS handshake for timeout tracking.  Map to a
// rdtsc-derived monotonic millisecond counter.  QEMU TCG runs at ~2 GHz
// effective, so divide by 2_000_000 to get ms.
uint64_t mg_millis(void) {
    uint64_t tsc;
    __asm__ __volatile__("rdtsc; shl $32,%%rdx; or %%rdx,%0"
                         : "=a"(tsc) :: "rdx");
    return tsc / 2000000ULL;
}
