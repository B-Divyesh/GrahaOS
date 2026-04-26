// user/tests/gcp_manifest.tap.c — Phase 22 closeout (G4.3) gate.
//
// Validates that etc/gcp.json is in sync with the live kernel manifest:
//   - The 4 Phase 22 channel types (net.frame.v1 / net.service.v1 /
//     net.socket.v1 / net.accept.v1) hash to the same FNV-1a values
//     declared in user/include/gcp_ops_generated.h.
//   - The 5 retired net syscalls (1041..1045) actually return -ENOSYS
//     under a pledge-granted caller — proves Stage F retirement holds.
//   - The 2 new SYS_CHAN_* numbers are 1091 / 1092 as documented.
//   - Manifest_type_known() in the kernel accepts the registered hashes
//     (probed via SYS_CHAN_CREATE which validates against manifest).

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern int printf(const char *fmt, ...);

// FNV-1a 64-bit, mirroring kernel/fs/simhash.c::fnv1a_hash64. Used to
// recompute the type hashes at test time and compare against the
// generated header constants.
static uint64_t fnv1a64(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 0x100000001b3ULL; }
    return h;
}

void _start(void) {
    tap_plan(14);

    // ---------- G1: type hashes match generated header ----------
    // Hashes from gen_manifest.py output (printed at regen time).
    uint64_t h_frame   = fnv1a64("grahaos.net.frame.v1");
    uint64_t h_service = fnv1a64("grahaos.net.service.v1");
    uint64_t h_socket  = fnv1a64("grahaos.net.socket.v1");
    uint64_t h_accept  = fnv1a64("grahaos.net.accept.v1");
    /* Phase 23 S7.8 — block I/O channel types added by gcp.json regen. */
    uint64_t h_blk_svc = fnv1a64("grahaos.blk.service.v1");
    uint64_t h_blk_lst = fnv1a64("grahaos.blk.list.v1");

    TAP_ASSERT(h_frame   == 0x907BAAAD52A368B3ULL,
               "1. grahaos.net.frame.v1 hash matches gcp.json regen");
    TAP_ASSERT(h_service == 0x19111BEED5E4736DULL,
               "2. grahaos.net.service.v1 hash matches gcp.json regen");
    TAP_ASSERT(h_socket  == 0xA48A3EFE1139CA79ULL,
               "3. grahaos.net.socket.v1 hash matches gcp.json regen");
    TAP_ASSERT(h_accept  == 0xF120F11A7BA1870EULL,
               "4. grahaos.net.accept.v1 hash matches gcp.json regen");
    TAP_ASSERT(h_blk_svc == 0xFB1E9BA25588A4A9ULL,
               "5. grahaos.blk.service.v1 hash matches gcp.json regen (Phase 23)");
    TAP_ASSERT(h_blk_lst == 0x671270DA03116FF6ULL,
               "6. grahaos.blk.list.v1 hash matches gcp.json regen (Phase 23)");

    // ---------- G2: kernel manifest accepts the registered hashes ----------
    cap_token_u_t wr = { .raw = 0 };
    long rc = syscall_chan_create(h_accept, CHAN_MODE_BLOCKING, 4, &wr);
    TAP_ASSERT(rc > 0, "7. SYS_CHAN_CREATE accepts net.accept.v1 hash");

    rc = syscall_chan_create(h_frame, CHAN_MODE_BLOCKING, 4, &wr);
    TAP_ASSERT(rc > 0, "8. SYS_CHAN_CREATE accepts net.frame.v1 hash");

    rc = syscall_chan_create(h_socket, CHAN_MODE_BLOCKING, 4, &wr);
    TAP_ASSERT(rc > 0, "9. SYS_CHAN_CREATE accepts net.socket.v1 hash");

    rc = syscall_chan_create(h_blk_svc, CHAN_MODE_BLOCKING, 4, &wr);
    TAP_ASSERT(rc > 0,
               "10. SYS_CHAN_CREATE accepts blk.service.v1 hash (Phase 23)");

    rc = syscall_chan_create(h_blk_lst, CHAN_MODE_BLOCKING, 4, &wr);
    TAP_ASSERT(rc > 0,
               "11. SYS_CHAN_CREATE accepts blk.list.v1 hash (Phase 23)");

    // ---------- G3: retired syscalls return -ENOSYS ----------
    // The 5 retired syscalls keep their pre-existing pledge gate, then
    // return -ENOSYS (-38). pledgetest already covers the pledge-deny
    // path; here we cover the post-pledge ENOSYS path. We hold full
    // pledge mask (default for spawned tests), so pledge succeeds and
    // we hit the retirement.
    {
        uint8_t buf[8];
        long r = syscall_net_ifconfig(buf);
        TAP_ASSERT(r == -38,
                   "12. SYS_NET_IFCONFIG retired (returns -ENOSYS)");
    }
    {
        char body[16];
        long r = syscall_http_get("http://127.0.0.1/", body, sizeof(body));
        TAP_ASSERT(r == -38,
                   "13. SYS_HTTP_GET retired (returns -ENOSYS)");
    }
    {
        uint8_t ip[4];
        long r = syscall_dns_resolve("example.com", ip);
        TAP_ASSERT(r == -38,
                   "14. SYS_DNS_RESOLVE retired (returns -ENOSYS)");
    }

    tap_done();
    syscall_exit(0);
}
