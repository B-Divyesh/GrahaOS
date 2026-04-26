// user/libnet/rawframe.h — Phase 22 Stage B.
//
// On-wire schema for the `/sys/net/rawframe` channel shared by a NIC driver
// daemon (producer: e1000d, future: virtiod) and netd (consumer). Each
// per-connection channel pair carries messages typed
// `grahaos.net.frame.v1`. Wire formats live here (not in the driver or
// daemon source) so test harnesses and future drivers can align.
//
// Flow:
//   1. Client (netd) connects to `/sys/net/rawframe`.
//   2. Driver's accept loop sees the new (rd_req, wr_resp) pair.
//   3. Driver sends an ANNOUNCE on wr_resp. Inline payload carries MAC +
//      slot geometry + link state. Message.handles[] carries two VMO
//      handles: the shared rx ring and the shared tx ring (allocated via
//      vmo_clone(VMO_CLONE_SHARED) so both sides have RW access to the
//      same physical pages).
//   4. Client sys_vmo_maps both handles into its address space.
//   5. Ongoing:
//        - RX: driver sends RX_NOTIFY{slot, len} (no handles) when a frame
//              has been DMA'd into the shared rx ring. Client reads from
//              that slot in its mapped rx-ring VA.
//        - TX: client writes a frame into its mapped tx-ring slot, then
//              sends TX_REQ{slot, len} on wr_req. Driver DMAs the frame.
//        - LINK_UP / LINK_DOWN: driver pushes to client on state change.
//
// Stage B scope: message types + ANNOUNCE exchange (VMO hand-off) land.
// Real RX/TX fanout is deferred to a follow-up sub-unit; it's safe to
// leave the kernel-proxy frame path as-is during Stages B-E per D8.

#pragma once

#include <stdint.h>

// Opcodes on wr_req (client → driver) and wr_resp (driver → client).
#define RAWFRAME_OP_ANNOUNCE     1u   // driver → client (with VMO handles)
#define RAWFRAME_OP_RX_NOTIFY    2u   // driver → client
#define RAWFRAME_OP_TX_REQ       3u   // client → driver
#define RAWFRAME_OP_LINK_UP      4u   // driver → client
#define RAWFRAME_OP_LINK_DOWN    5u   // driver → client

// ANNOUNCE payload. Sent immediately after the rawframe accept succeeds.
// Carries everything the client needs to start consuming frames:
//   - MAC address (copied from the NIC's EEPROM by the driver)
//   - Shared ring geometry (slot count + bytes per slot)
//   - Initial link state
//
// handles[0] = rx ring VMO (driver writes, client reads)
// handles[1] = tx ring VMO (client writes, driver reads)
typedef struct __attribute__((packed)) rawframe_announce {
    uint8_t  op;                 // = RAWFRAME_OP_ANNOUNCE
    uint8_t  mac[6];
    uint8_t  link_up;            // 0 or 1
    uint32_t slot_count;         // e.g. 16
    uint32_t slot_size;          // e.g. 4096
    uint32_t reserved;           // future: MTU, extended flags
} rawframe_announce_t;

_Static_assert(sizeof(rawframe_announce_t) == 20,
               "rawframe_announce_t layout drift");

// RX_NOTIFY / TX_REQ / LINK_* share a compact header.
typedef struct __attribute__((packed)) rawframe_slot_msg {
    uint8_t  op;                 // RX_NOTIFY / TX_REQ / LINK_UP / LINK_DOWN
    uint8_t  _pad[3];
    uint32_t slot;               // Slot index within the shared ring
    uint32_t length;             // Frame bytes at that slot (0 for LINK_*)
} rawframe_slot_msg_t;

_Static_assert(sizeof(rawframe_slot_msg_t) == 12,
               "rawframe_slot_msg_t layout drift");
