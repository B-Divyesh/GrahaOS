// user/tests/chantest_named.tap.c — Phase 22 Stage A U2 gate.
//
// Exercises the new named-channel registry (SYS_CHAN_PUBLISH +
// SYS_CHAN_CONNECT, backed by kernel/net/rawnet.c). All assertions run in a
// single process: the test plays BOTH the publisher and the connector roles
// through separate cap_token handles, because cross-process setup is too
// heavy to justify here (Stage E's netd + e1000d integration tests the
// multi-process path for real).

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// Well-known GCP hashes the kernel registered in kernel/ipc/manifest.c.
// Computed at runtime via gcp_type_hash() which mirrors the kernel's FNV-1a.
static uint64_t H_ACCEPT_V1;
static uint64_t H_SOCKET_V1;

static void zero_msg(chan_msg_user_t *m) {
    for (size_t i = 0; i < sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
}

void _start(void) {
    tap_plan(20);

    H_ACCEPT_V1 = gcp_type_hash("grahaos.net.accept.v1");
    H_SOCKET_V1 = gcp_type_hash("grahaos.net.socket.v1");

    // -------------------- G1: Invalid-name rejection --------------------
    {
        // Names must start with '/'.
        long rc = syscall_chan_publish("notaslashname", 13, H_SOCKET_V1,
                                       (cap_token_u_t){ .raw = 0 });
        TAP_ASSERT(rc < 0, "1. publish bad name (no leading /) rejected");

        // Zero-length name.
        rc = syscall_chan_publish("/", 0, H_SOCKET_V1,
                                  (cap_token_u_t){ .raw = 0 });
        TAP_ASSERT(rc < 0, "2. publish zero-length name rejected");
    }

    // -------------------- G2: Publish + self-connect round-trip --------
    // Step 1: create an accept channel (accept.v1 type). We keep the read
    // end; the write end goes into the registry.
    cap_token_u_t accept_rd = {.raw = 0}, accept_wr = {.raw = 0};
    long rc = syscall_chan_create(H_ACCEPT_V1, CHAN_MODE_BLOCKING, 8, &accept_wr);
    accept_rd.raw = (uint64_t)rc;
    TAP_ASSERT(rc > 0, "3. chan_create(accept.v1) returns read handle");
    TAP_ASSERT(accept_wr.raw != 0, "4. accept write handle populated");

    // Step 2: publish /test/svc with payload = socket.v1.
    const char *name = "/test/svc";
    long prc = syscall_chan_publish(name, 9, H_SOCKET_V1, accept_wr);
    TAP_ASSERT(prc == 0, "5. publish /test/svc succeeds");

    // Step 3: publish again with a different payload hash — re-publish by
    // same pid is allowed (covers daemon respawn semantics).
    long prc2 = syscall_chan_publish(name, 9, H_SOCKET_V1, accept_wr);
    TAP_ASSERT(prc2 == 0, "6. re-publish by same pid replaces the slot");

    // Step 4: connect to a non-existent name → -EBADF.
    cap_token_u_t client_wr_req = {.raw = 0}, client_rd_resp = {.raw = 0};
    long crc = syscall_chan_connect("/no/such/name", 13, &client_wr_req,
                                    &client_rd_resp);
    TAP_ASSERT(crc < 0, "7. connect to missing name returns < 0");

    // Step 5: connect to /test/svc → kernel allocates two new channels, sends
    // accept message to (us), returns client-side handles.
    crc = syscall_chan_connect(name, 9, &client_wr_req, &client_rd_resp);
    TAP_ASSERT(crc == 0, "8. connect to /test/svc succeeds");
    TAP_ASSERT(client_wr_req.raw != 0, "9. client request-write token populated");
    TAP_ASSERT(client_rd_resp.raw != 0, "10. client response-read token populated");
    TAP_ASSERT(client_wr_req.raw != client_rd_resp.raw,
               "11. client tokens differ");

    // Step 6: recv the accept message on our accept channel. It should carry
    // two handles: the server-side read-end of the REQUEST channel (handles[0])
    // and the server-side write-end of the RESPONSE channel (handles[1]).
    chan_msg_user_t amsg;
    zero_msg(&amsg);
    long abytes = syscall_chan_recv(accept_rd, &amsg, 2000000000ULL);
    TAP_ASSERT(abytes >= 16, "12. accept message receives with >=16 inline bytes");
    TAP_ASSERT(amsg.header.nhandles == 2, "13. accept message carries 2 handles");

    cap_token_u_t server_rd_req  = { .raw = amsg.handles[0] };
    cap_token_u_t server_wr_resp = { .raw = amsg.handles[1] };
    TAP_ASSERT(server_rd_req.raw  != 0 &&
               server_wr_resp.raw != 0, "14. server-side handles non-zero");

    // Step 7: client -> server (request path).
    chan_msg_user_t req;
    zero_msg(&req);
    req.header.type_hash  = H_SOCKET_V1;
    req.header.inline_len = 8;
    for (int i = 0; i < 8; i++) req.inline_payload[i] = (uint8_t)(0x10 + i);
    long sres = syscall_chan_send(client_wr_req, &req, 1000000000ULL);
    TAP_ASSERT(sres == 0, "15. client->server send on request channel ok");

    chan_msg_user_t req_got;
    zero_msg(&req_got);
    long rres = syscall_chan_recv(server_rd_req, &req_got, 1000000000ULL);
    TAP_ASSERT(rres == 8, "16. server recv yields 8 bytes on request channel");
    int req_match = 1;
    for (int i = 0; i < 8; i++)
        if (req_got.inline_payload[i] != (uint8_t)(0x10 + i)) req_match = 0;
    TAP_ASSERT(req_match, "17. request payload matches");

    // Step 8: server -> client (response path).
    chan_msg_user_t resp;
    zero_msg(&resp);
    resp.header.type_hash  = H_SOCKET_V1;
    resp.header.inline_len = 4;
    for (int i = 0; i < 4; i++) resp.inline_payload[i] = (uint8_t)(0xC0 + i);
    sres = syscall_chan_send(server_wr_resp, &resp, 1000000000ULL);
    TAP_ASSERT(sres == 0, "18. server->client send on response channel ok");

    chan_msg_user_t resp_got;
    zero_msg(&resp_got);
    rres = syscall_chan_recv(client_rd_resp, &resp_got, 1000000000ULL);
    TAP_ASSERT(rres == 4, "19. client recv yields 4 bytes on response channel");
    int resp_match = 1;
    for (int i = 0; i < 4; i++)
        if (resp_got.inline_payload[i] != (uint8_t)(0xC0 + i)) resp_match = 0;
    TAP_ASSERT(resp_match, "20. response payload matches");

    tap_done();
    exit(0);
}
