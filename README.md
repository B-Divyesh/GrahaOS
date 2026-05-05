# GrahaOS

> *A from-scratch x86_64 operating system designed as an AI-native substrate.*
>
> **Phase 27 complete · gate 987/987 × 3-iter KVM clean · Phase 28 (1-hour fault-injection soak) is the next milestone.**

<p align="center">
  <img src="docs/diagrams/anim-control-loop.svg" alt="GrahaOS request loop: snapshot → dispatch → cap-check → audit, with transactional rollback" width="100%"/>
</p>

<p align="center">
  <img src="docs/diagrams/anim-phase-progress.svg" alt="GrahaOS phase progress: Phase 27 complete, Phase 28 next" width="100%"/>
</p>

[![GrahaOS Control Protocol overview](https://img.youtube.com/vi/uivT1Bw1-l0/0.jpg)](https://www.youtube.com/watch?v=uivT1Bw1-l0 "GrahaOS Control Protocol overview")

---

## What GrahaOS Actually Is

GrahaOS is a **clean-room operating system written from the first commit for x86_64**, intended as a substrate on which AI agents can act safely. We did not start from Linux, BSD, MINIX, or seL4 — every line of the kernel, the userspace, the filesystem, the network stack, and the test harness was authored in this tree. The pitch is a single sentence: *the OS treats AI agents as unprivileged userspace processes, capability-narrowed and pledge-restricted, that can speculate freely against versioned filesystem state and roll back atomically when they're wrong.*

What that means in practice, as the system stands today:

- **Capabilities are the sole authority.** No `uid`, no `gid`, no `root`, no ambient access of any kind. Every syscall checks either a capability token (one of 14 kinds, with 17 distinct rights bits, generation-counted to detect use-after-revoke) or a pledge class (one of 14 monotonically narrowing classes inspired by OpenBSD pledge but composable with channel handle transfer). [`kernel/cap/`](kernel/cap/)
- **One public manifest.** [`/etc/gcp.json`](etc/gcp.json) declares every syscall, every channel message type, every audit event, and every capability kind/right. The kernel parses it, userspace reads it, the WIT generator (`scripts/gcp2wit.py`) produces WebAssembly Component-Model bindings from it, and the manifest_blob is exported to processes via `SYS_MANIFEST_EXPORT`. AI agents and human developers read the same document.
- **Speculation is a first-class primitive.** `SYS_SNAP_CREATE` (1093) takes a copy-on-write snapshot of process memory, FD state, pledge mask, and pinned filesystem versions; `SYS_TXN_BEGIN/COMMIT/ABORT` (1098–1100) wraps that into transactional regions where external channel sends are buffered and replayed on commit or discarded on abort. [`kernel/snap/`](kernel/snap/) [`kernel/txn/`](kernel/txn/)
- **Drivers drift outward.** The kernel keeps VMM, scheduler, interrupts, framebuffer, timer, and the capability/channel primitives. Everything else — AHCI (`ahcid`), e1000 NIC (`e1000d`), TCP/IP (`netd`), the WebAssembly host (`wasmd`) — runs in userspace and speaks the same channel protocol any other process does. The kernel side of `ahci.c` collapsed from 633 LOC to 152 in Phase 24a.
- **Verified by hand and by harness.** Every spec from Phase 12 onward includes a `manual_verification` section with copy-pasteable shell commands and expected output. Every phase ships a TAP-1.4 test gate (`make test`); the gate currently runs **987 assertions across ~64 test binaries × 3 iterations under KVM** without a single regression. The `make test-sentinel-meta` target exists specifically to prove the harness catches failures.

The full technical report is at [`docs/GRAHAOS-COMPREHENSIVE-REPORT.md`](docs/GRAHAOS-COMPREHENSIVE-REPORT.md) (≈97k words, 11 chapters, 4 appendices). This README is the entry point.

---

## Why Not Just Bolt AI Onto Unix?

The pre-existing AI-OS attempts (Rutgers AIOS, MemGPT, agentic shells layered over Linux) start from POSIX and add a coordinator. GrahaOS argues that POSIX is precisely the wrong substrate for agentic execution:

- **Ambient authority.** A POSIX process inherits `uid` and the entire syscall surface; sandboxing is a retrofit (seccomp-bpf, namespaces, gVisor) and every retrofit ships its own CVE history. GrahaOS makes capability checks the *only* admission path.
- **No rollback primitive.** When a Unix agent does the wrong thing, you cannot undo it. ZFS/Btrfs snapshots are filesystem-only and do not capture process state. GrahaOS snapshots capture pages + FDs + pledge + FS pins atomically and restore them by reverting per-page PTE state, FD slots, and grahafs version chains.
- **No machine-readable interface.** POSIX is documented in prose. GrahaOS's `/etc/gcp.json` is the kernel's own declared interface; the kernel and userspace cannot disagree about what a syscall does.
- **Drivers are kernel-trusted.** A buggy driver on Linux takes down the kernel. GrahaOS drivers are Ring-3 processes; when `ahcid` dies, the kernel's `blk_client` reaps the channel, restores HBA state via the `ahci_restore_after_userdrv_death` substrate, and the next spawn picks up where the dead one left off (verified by `userdrv_respawn_stress` 10/10 in the gate).

These are concrete design decisions, not slogans. The audit findings that motivated Phase 12+ — the 48 KB filesystem ceiling on the v1 GrahaFS, all-`NULL` CAN callbacks, 41 % of the codebase being vendored Mongoose, no journaling — are documented in [`CLAUDE.md`](CLAUDE.md) and have all been closed by phases 13–22.

---

## System Architecture

<p align="center">
  <img src="docs/diagrams/fig-architecture.svg" alt="GrahaOS hardware → kernel → user partition" width="100%"/>
</p>

The kernel is a **modular monolith transitioning to a hybrid microkernel**. Boot path: Limine 9.3.3 hands off → `kmain` initialises PMM (bitmap with per-page refcount), VMM (PML4 walk via `g_hhdm_offset`), interrupts, LAPIC timer, SMP bring-up via INIT-SIPI-SIPI, capability core, channel registry, snapshot/transaction subsystems, audit, then forks `bin/init` (PID 1). `init` reads `/etc/init.conf`, spawns `ahcid`, `e1000d`, `netd`, and `wasmd` as capability-narrowed userspace daemons, then drops into `gash` (the GrahaOS shell) or `ktest` (the gate harness) per the `autorun=` cmdline flag.

The seven design principles are non-negotiable; every spec in [`specs/`](specs/) must uphold at least two and violate none:

1. **Capabilities are the sole authority.** No ambient authority anywhere. Every syscall checks a capability token or a pledge class.
2. **GCP is the only native public interface.** Every syscall, channel message, and audit event has an entry in `/etc/gcp.json`.
3. **Foundation before migration.** New primitives ship before consumers move to them; old paths live briefly alongside as shims.
4. **Failure is auditable, not silent.** Panics dump structured oops frames; capability violations log subject/object/rights.
5. **Everything is testable by hand.** Every spec has a `manual_verification` section.
6. **Drivers drift outward.** AHCI, NIC, network stack, WASM host — all userspace.
7. **Speculation is a first-class AI primitive.** Snapshots + transactions + versioned FS, atomic.

---

## Subsystem Tour

### Capability Core

<p align="center">
  <img src="docs/diagrams/fig-cap-layout.svg" alt="Capability token layout: gen:32 | idx:24 | flags:8" width="80%"/>
</p>

A `cap_token_t` is 64 bits — `gen:32 | idx:24 | flags:8`. The `idx` indexes a per-process handle table; the `gen` is incremented on revoke so a stale token resolves to a different `cap_object_t` and fails. `cap_object_t` is 96 bytes, holds the kind (one of 14: `CAN`, `MEMORY`, `CHANNEL_ENDPOINT`, `VMO`, `STREAM`, `WASM_INSTANCE`, `TRANSACTION`, `SYSTEM`, `SNAPSHOT`, `CONSOLE`, …), the rights mask (17 bits), the audience (up to 8 PIDs), and lock-free hot-path resolve via per-table generation. Rights compose by intersection on derive; `cap_object_derive_quiet` skips audit emission for inheritance walks. Source: [`kernel/cap/token.h`](kernel/cap/token.h), [`kernel/cap/object.c`](kernel/cap/object.c), [`kernel/cap/handle_table.c`](kernel/cap/handle_table.c).

Pledge is composable narrowing layered on top of capabilities: `SYS_PLEDGE` accepts a class mask that monotonically shrinks (you can never re-grant) plus an optional `wasm_pledge_narrow_args_t` that the kernel uses to narrow capabilities post-spawn for WebAssembly modules. 14 classes — `IPC_SEND`, `IPC_RECV`, `FS_READ`, `FS_WRITE`, `NET_BIND`, `NET_CONNECT`, `SPAWN`, `MMAP`, `SYS_CONTROL`, `SYS_QUERY`, `TIME`, `SIGNAL`, `EXEC`, `RANDOM` — with a `PLEDGE_MIN_ALLOWED` floor that keeps the process able to exit and emit audit records. [`kernel/cap/pledge.c`](kernel/cap/pledge.c)

### Capability Activation Network (CAN)

<p align="center">
  <img src="docs/diagrams/fig-can.svg" alt="CAN: switches as the registry primitive, dependency layers, cycle detection" width="85%"/>
</p>

Every kernel subsystem registers as a *switch* in the Capability Activation Network — a directed dependency graph the boot path resolves with cycle detection (Lex → Resolve → TypeCheck → Cycles → Reachability → Optimize). Activating a switch cascades to its prerequisites; deactivating it cascades the other way. This is what replaced the Phase-11 `NULL` callbacks and is the substrate any future driver/service plugs into. The full architecture is in [`ACTIVATION_ARCHITECTURE.md`](ACTIVATION_ARCHITECTURE.md).

### Channels & VMOs

<p align="center">
  <img src="docs/diagrams/fig-channel-vmo.svg" alt="Typed bidirectional channels with VMO handle passing" width="85%"/>
</p>

Channels are typed bidirectional IPC endpoints. Every message carries an FNV-1a 64-bit type-hash that is validated against the manifest at `chan_send` time — sending the wrong message type to a peer expecting a specific protocol is a kernel-side reject, not a userspace bug. Handles (capabilities, VMO maps, channel endpoints) ride inline with the message and are remapped into the receiver's address space by `chan_marshal_recv`. VMOs (Virtual Memory Objects) come in six flavours — `zeroed`, `ondemand`, `cow`, `pinned`, `mmio`, `contiguous` — with the COW path serving snapshots and the MMIO path serving userspace drivers. [`kernel/ipc/channel.c`](kernel/ipc/channel.c), [`kernel/mm/vmo.c`](kernel/mm/vmo.c)

### GCP — the Manifest as Single Public Interface

<p align="center">
  <img src="docs/diagrams/fig-gcp.svg" alt="GCP manifest structure: types, syscalls, ops, audit_events" width="80%"/>
</p>

[`/etc/gcp.json`](etc/gcp.json) is the contract. It declares 100 syscall slots (1001–1100), 12 named channel services (`grahaos.wasm.service.v1`, etc.), 55 audit event codes, and the full capability ABI. The kernel embeds a hashed copy of it (FNV-1a 64-bit `gen` field) at build time via `scripts/gen_manifest_blob.py`; processes pull it back via `SYS_MANIFEST_EXPORT` (1112) so a sandboxed agent can introspect the OS interface without out-of-band knowledge. The same file feeds [`scripts/gcp2wit.py`](scripts/gcp2wit.py) which emits `etc/gcp.wit` (186 lines, kebab-case identifiers, per-pledge-class interface error variants) for WebAssembly Component-Model consumers.

### Submission Streams

<p align="center">
  <img src="docs/diagrams/fig-streams.svg" alt="SQE/CQE rings with manifest-gated dispatch" width="80%"/>
</p>

Async I/O without a thread pool. Submission Queue Entry / Completion Queue Entry rings, mapped via shared VMO between caller and worker, dispatched through the manifest. Semantics are intentionally close to io_uring (Axboe 2019), but every dispatch is capability-gated and the available op set is whatever `/etc/gcp.json` declares. `stream_reap` loops internally until `min_complete` or deadline (a Phase-23 fix that closed a class of cross-CPU wake races). [`kernel/io/stream.c`](kernel/io/stream.c)

### Scheduler & SMP

<p align="center">
  <img src="docs/diagrams/fig-sched.svg" alt="Per-CPU runqueues with cross-CPU IPI doorbell" width="85%"/>
</p>

Per-CPU runqueues with work-stealing, seL4-style cross-CPU IPI doorbell on wake (`sched_maybe_doorbell_ipi`), `INT 49` same-CPU yield, starvation detection. `sched_block_on_channel` / `sched_wake_one_on_channel` is the canonical IPC wait/wake pair. The scheduler skips `ZOMBIE` tasks at runq dequeue (a fix that landed during FU27.WASM Stage D2 to keep `SIGKILL` deterministic). The SMP bring-up at boot uses INIT-SIPI-SIPI; APs join with a per-CPU `runq_t` that takes a thread on first `schedule()`. [`arch/x86_64/cpu/sched/sched.c`](arch/x86_64/cpu/sched/sched.c), [`arch/x86_64/cpu/smp.c`](arch/x86_64/cpu/smp.c)

### GrahaFS v2 — Versioned Segments

<p align="center">
  <img src="docs/diagrams/fig-grahafs.svg" alt="GrahaFS v2 layout: 512-byte inode, journal, segments, version chain" width="85%"/>
</p>

GrahaFS v2 replaced the v1 48-KB-ceiling filesystem in Phase 19. 512-byte inodes with direct + indirect + double-indirect pointers (4 GB max file size), 64 MB journal with two-barrier protocol (transaction begin → write → commit → fsync), version chains per inode capped at 16 entries, immutable-once-written segments with background GC, and **AI metadata at the inode level** — every inode carries a 128-dimension embedding slot (only `embedding[0]` used today, holding the 64-bit SimHash) plus a 4-byte cluster ID. Sequential Leader Clustering (Hamming threshold τ=10) groups files in-memory at mount time. [`kernel/fs/grahafs_v2.c`](kernel/fs/grahafs_v2.c), [`kernel/fs/journal.c`](kernel/fs/journal.c), [`kernel/fs/cluster.c`](kernel/fs/cluster.c)

### Snapshots — COW Process State

<p align="center">
  <img src="docs/diagrams/fig-snapshot.svg" alt="Snapshot capture: barrier, PML4 walk, FD/pledge capture, FS version pin" width="85%"/>
</p>

A snapshot is a barrier-synchronised capture of one or more processes' state. `snap_begin_barrier` does an atomic CAS + IPI broadcast, polls each CPU's runq for `current==idle_task`, with a 100 ms TSC watchdog → `-ETIME`. Inside the barrier, `snap_walk_user_half` deep-walks the user-half PML4 of every in-scope task, bumps a per-physical-page COW tracker + `pmm_page_ref`, records `(virt, phys, flags)`, and clears `PTE_WRITABLE` so subsequent writes COW into fresh pages. `snap_capture_fs_pins_for_task` walks the task's FD table and calls `grahafs_pin_version` for every open file inode. Channels in the freeze set are walked under `g_chan_reg_lock` and have their endpoints flipped to a `frozen_at_snap` state that returns `-EFROZEN` from `chan_send`/`chan_recv`. Restore replays per-page `(virt, phys, flags)` into the live CR3, restores regs/FDs/pledge for non-caller tasks, and reverts grahafs version chains. The 10 000-cycle `snap_stress_cycle` test in the gate runs in ≈8.2 s with 0 bytes of kheap delta. [`kernel/snap/`](kernel/snap/)

### Transactional Speculation

<p align="center">
  <img src="docs/diagrams/fig-txn.svg" alt="Transaction state machine: ACTIVE → COMMITTING/ABORTING → COMMITTED/ABORTED" width="85%"/>
</p>

Transactions wrap snapshots with external-effect buffering. `txn_begin` takes a snapshot and seeds a per-transaction VMO ring (lazy-allocated 4 MiB default, magic-bookended `BEADF00D`/`DEADC0DE` records). Inside the transaction, `chan_send` to a peer outside the transaction's scope is *buffered* — the message goes into the ring instead of the live channel. On `commit`, the ring is drained via a replay engine that sets `caller->replay_in_progress=1` so the prologue doesn't re-buffer; on `abort`, the ring is freed and the snapshot is restored. State transitions (`ACTIVE→COMMITTING→COMMITTED`, `ACTIVE→ABORTING→ABORTED`, `COMMITTING_FAILED→ABORTING`) use `__atomic_compare_exchange_n` for serialisation, with a per-transaction waitqueue + 100 ms watchdog so an abort racing a commit-in-flight either wins (if `COMMITTING_FAILED`) or sees `-EALREADY`. Up to 4 transactions nest. The 1000-cycle `txn_stress_basic` test runs each iteration with 10 in-scope sends → drain → commit. [`kernel/txn/transaction.c`](kernel/txn/transaction.c), [`kernel/txn/buffer.c`](kernel/txn/buffer.c), [`kernel/txn/replay.c`](kernel/txn/replay.c)

### Wasmtime + GCP-as-WIT

<p align="center">
  <img src="docs/diagrams/fig-wasm.svg" alt="wasmd daemon: Wasmtime substrate with capability-narrowed module spawn" width="85%"/>
</p>

WebAssembly hosting via [wasm3](https://github.com/wasm3/wasm3) v0.5.0 vendored at [`vendor/wasm3/`](vendor/wasm3/). The `wasmd` daemon publishes `/sys/wasm/control` via libnet, accepts `RUN_MODULE` requests, drives wasm3 `ParseModule`/`LoadModule`/`LinkRawFunction(gcp.print)`/`FindFunction(_start)`/`CallV`, captures stdout via the `host_gcp_print` shim, and reports per-call summary back to the client. Five hand-rolled fixture modules ship in the initrd: `hello.wasm` (90 B with print), `oopsie.wasm` (37 B unreachable trap), `long_running.wasm` (44 B infinite loop), `fn_call_bench.wasm` (1076 B 512 calls), `demo_net.wasm` (79 B net.send). The `grahai wasm-run <path>` verb runs a module end-to-end and reports the audit-event delta. **Capability narrowing is post-spawn**: `SYS_PLEDGE` with `PLEDGE_FLAG_NARROW_EXEC` shrinks both the pledge mask and the capability set the child inherits, so a WASM module can be granted `IPC_SEND` to one specific channel and nothing else. Four gate test files exercise it: `wasm_e2e_run` (4/4), `wasm_fault_trap` (3/3 — daemon survives traps), `wasm_load_reject` (3/3 — bad-magic + truncated), `wasm_concurrent_serial` (3/3 — per-call cleanup). [`user/wasmd/wasmd.c`](user/wasmd/wasmd.c)

### Userspace Drivers

<p align="center">
  <img src="docs/diagrams/fig-userdrv.svg" alt="Userspace driver substrate: drv_register, drv_irq_wait, mmio_vmo_create" width="85%"/>
</p>

`drv_register` / `drv_irq_wait` / `mmio_vmo_create` is the substrate. A driver process registers an IRQ vector with the kernel, gets back an SPSC IRQ ring (with coalesced doorbell — one wake per IRQ batch), and maps device MMIO via a VMO. **`e1000d`** (Phase 21) drives the Intel 82540EM in 1700 LOC of userspace; the kernel-resident driver is now 70 LOC of capability glue. **`ahcid`** (Phase 23) drives SATA via AHCI; the kernel side is 152 LOC after Phase 24a's strip. The respawn pattern is verified: `ahcid_die` followed by `init` re-spawn followed by `ahci_restore_after_userdrv_death` recovers the HBA without losing in-flight requests. The `userdrv_respawn_stress` test in the gate runs 10 iterations of die-respawn-resume cleanly.

### Network Stack

The in-tree network daemon `netd` implements RFC-faithful Eth/ARP/IPv4/ICMP/UDP/TCP-Reno/DHCP/DNS in roughly 4 000 LOC of userspace (replacing the 19 141-line vendored Mongoose dropped in Phase 22). Client-facing surface is `libnet` (sockets), `libhttp` (request/response), `libtls-mg` (TLS shim). The `wasmd` daemon, the `grahai` agent, and the `wasm` CLI all consume `libnet`. Source: [`user/netd.c`](user/netd.c) plus [`user/drivers/netd_*.c`](user/drivers/).

### TUI Framework + Virtual Consoles (Phase 27)

Frame compositing, keyboard/mouse routing, four virtual consoles (Alt+1..Alt+4), 256-colour xterm-compatible palette, 11 Unicode box-drawing glyphs, sprite cells (1 791 slots), RGBA bitmap overlays. The `libtui` userspace library provides `tui_init`/`tui_attach`/`tui_clear`/`tui_write_cell`/`tui_print`/`tui_draw_box`/`tui_set_cursor`/`tui_present`. The `fbd` compositor is the substrate for graphical-mode applications; under `autorun=ktest` it stays inert and the substrate is exercised via DEBUG syscalls. [`kernel/console/console.c`](kernel/console/console.c), [`user/libtui/`](user/libtui/), [`user/fbd/fbd.c`](user/fbd/fbd.c).

### Audit & Observability

256-byte audit records, 55 event types (capability derive/revoke, pledge narrow, transaction begin/commit/abort, snapshot create/delete, syscall rate-limit, plan-step, …), on-disk daily rotation at UTC, queryable via `SYS_AUDIT_QUERY`. `SYS_AUDIT_SUBSCRIBE` (1110) + `SYS_AUDIT_STREAM_READ` (1111) give a process a 64-entry SPSC ring of live events — used by `grahai` to stream agent activity for replay. Distinct from `klog`, which is the in-memory lossy debug ring used during boot/panic. [`kernel/audit.c`](kernel/audit.c)

---

## What's Actually Shipped

| Subsystem | Files | LOC | Tests |
|---|---|---|---|
| Kernel + arch | 89 C files | ≈58 000 lines (excl. wasm3) | gated by 64-test TAP harness |
| Userspace | 156 C files | covers init, gash, grahai, wasmd, netd, e1000d, ahcid, libnet, libhttp, libtls-mg, libtui, libdriver, ktest, 40+ test binaries | — |
| Vendored | wasm3 v0.5.0 (≈13 000 LOC) | MIT-licensed | — |
| Specs | 27 Phase YAMLs (Phase 12 → 28) + 4 follow-up registries | — | every spec has `acceptance_criteria` + `manual_verification` |
| Manifest | [`/etc/gcp.json`](etc/gcp.json) | 100 syscall slots, 12 channel services, 55 audit codes | host tests: `gcp2wit_deterministic`, `gcp2wit_completeness` |
| Gate | **987 assertions × 64 binaries × 3 KVM iterations** | 0 regressions | `make test` |

Major subsystems by LOC (kernel-side):

- VMM + PMM: ≈2 200 LOC
- Capability core (token/object/handle_table/pledge/audit/CAN/system/wasm): ≈3 800 LOC
- Channel + VMO: ≈2 600 LOC
- GrahaFS v2 + journal + segment + GC + simhash + cluster: ≈4 100 LOC
- Snapshot + capture + restore + barrier + cow_fault + chan_freeze: ≈1 950 LOC
- Transaction + buffer + replay: ≈1 250 LOC
- Audit: ≈900 LOC
- Console (Phase 27 TUI + sprites + gfx overlay): ≈700 LOC

---

## Build & Run

### Prerequisites (Fedora 40+)

```bash
sudo dnf install gcc-c++ make git nasm bison libelf-devel xorriso qemu \
                 python3 python3-pyyaml
```

You also need an `x86_64-elf` cross-toolchain (GCC 15.x + Binutils 2.x with gold). The build expects it at `/home/atman/GrahaOS/toolchain/bin/x86_64-elf-{gcc,ld,as}`. Build instructions from the [OSDev wiki cross-compiler page](https://wiki.osdev.org/GCC_Cross-Compiler) work as-is.

### Build, run, test

```bash
git clone https://github.com/B-Divyesh/GrahaOS.git
cd GrahaOS

# Build the kernel + userspace + ISO
make clean
make

# Run interactively under QEMU (drops into gash)
make qemu-interactive

# Run the full TAP gate (KVM-accelerated by default; falls back to TCG
# if /dev/kvm is absent or GRAHAOS_TEST_NO_KVM=1 is set)
make test

# Prove the harness catches a deliberate failure
make test-sentinel-meta

# Reformat the AHCI disk image (required after FS on-disk format changes)
make reformat
```

Expected output of `make test`:

```
[1..] sentinel_pass: 2/2 ok
[2..] libtap_selftest: 5/5 ok
[3..] libctest: 37/37 ok
...
[64..] wasm_concurrent_serial: 3/3 ok
# TAP DONE
SUMMARY: 987 assertions, 0 failed, 64 binaries, 64 passed
```

### Useful gsh (the GrahaOS shell) commands

```text
gsh> caps                    # list this process's held capabilities
gsh> pledge ipc_send,fs_read # narrow this process's pledge mask
gsh> snapshot                # create a snapshot, print the handle
gsh> snapshots               # list active snapshots
gsh> restore <handle>        # restore from a snapshot handle
gsh> txn { ... } commit      # transactional region with explicit commit
gsh> txn { ... } abort       # … or abort
gsh> ifconfig                # netd interfaces
gsh> ping 1.1.1.1            # in-tree ICMP
gsh> simhash <path>          # 64-bit fingerprint of a file
gsh> similar <path>          # files within Hamming distance 10
gsh> clusters                # cluster table
gsh> grahai wasm-run /etc/wasm/hello.wasm   # run a wasm module
gsh> ktest <name>            # run a single TAP test interactively
```

---

## Repository Tour

```
GrahaOS/
├── arch/x86_64/            CPU, MMU, drivers (boot-side), interrupts, syscall entry
│   ├── cpu/                sched, smp, syscall, interrupts
│   ├── mm/                 PMM, VMM
│   └── drivers/            keyboard, ioapic, lapic_timer, e1000, ahci (slimmed)
├── kernel/                 OS-agnostic kernel core
│   ├── cap/                tokens, objects, handle_table, pledge, audit, CAN, system, wasm
│   ├── ipc/                channels, manifest blob, rawnet
│   ├── mm/                 VMO
│   ├── fs/                 GrahaFS v2, journal, segment, GC, simhash, cluster, pipe, blk_client
│   ├── snap/               snapshot, capture, restore, barrier, cow_fault, chan_freeze
│   ├── txn/                transaction, buffer, replay
│   ├── io/                 submission streams (SQE/CQE)
│   ├── console/            virtual consoles, sprites, gfx overlay
│   ├── driver/             userdrv substrate (drv_register/IRQ ring)
│   ├── resource/           rlimits + token-bucket I/O
│   ├── audit.c             audit log, 55 event codes, subscriber rings
│   └── main.c              kmain
├── user/                   userspace
│   ├── init.c              PID 1
│   ├── gash.c              shell
│   ├── grahai.c            AI agent CLI (legacy + wasm-run)
│   ├── wasmd/              WebAssembly host daemon (wasm3 inside)
│   ├── netd.c              TCP/IP daemon
│   ├── drivers/            e1000d, ahcid, netd_*
│   ├── libnet/ libhttp/ libtls-mg/ libtui/ libdriver/
│   ├── ktest.c             TAP harness front-end
│   └── tests/              ≈40 gate test binaries + 5 deferred
├── vendor/wasm3/           wasm3 v0.5.0 (MIT)
├── etc/                    gcp.json (the manifest), gcp.wit (generated), init.conf
├── scripts/                run_tests.sh, parse_tap.py, gcp2wit.py,
│                           gen_manifest_blob.py, gen_wasm_fixtures.py, mkfs.gfs.c
├── specs/                  Phase 12–28 YAMLs + 4 follow-up registries + schema.yml
├── problems/               per-phase problems_faced.md + audit_log.json
├── docs/
│   ├── GRAHAOS-COMPREHENSIVE-REPORT.md   ≈97k-word technical report
│   └── diagrams/           18 SVG figures + 2 animated SVGs
├── ROADMAP.md              phase-by-phase plan
├── ACTIVATION_ARCHITECTURE.md   the original CAN design document
├── CLAUDE.md               development workflow + current state banner
└── README.md               this file
```

---

## Roadmap

GrahaOS development is organised into spec-driven phases. Every phase has a YAML in [`specs/`](specs/) covering scope, kernel structs, syscalls, tests, manual verification steps, and exit criteria. The phase cannot close until every gate test is green and every exit criterion is met.

**Done:**

- **Phase 0–11b** — toolchain, framebuffer, MMU, ring 0/3 split, VFS, SMP, AHCI, malloc, spawn/signal, CAN v1, AI metadata, e1000, Mongoose+TLS, FD table, pipes, SimHash, sequential leader clustering.
- **Phase 12** — TAP-1.4 test harness with sentinel-meta + parse_tap.
- **Phase 13–14** — structured logging, slab allocator, per-CPU magazines.
- **Phase 15a–b** — capability objects v2, pledge, audit (44 events at first, 55 today).
- **Phase 16–18** — CAN callbacks, channels + VMOs, submission streams (io_uring-style).
- **Phase 19** — GrahaFS v2 (versioned segments, 4 GB files, 64 MB journal).
- **Phase 20** — per-CPU scheduler, work-stealing, RLIMITs, token-bucket I/O.
- **Phase 21–23** — userspace drivers (`e1000d`, `ahcid`), in-tree TCP/IP, Mongoose deletion.
- **Phase 24 + 24a** — COW snapshots, microsecond-tier IPC (state-conditional doorbell IPI, SPSC ring with coalesced doorbell, batched FS ops, KVM enable, TSC-calibrated busy-waits).
- **Phase 25** — transactional speculation (begin/commit/abort + buffered external sends + replay).
- **Phase 26** — Wasmtime substrate + `gcp2wit.py` + manifest export + cap inheritance.
- **Phase 27** — TUI framework, virtual consoles, sprites, gfx overlay, AI primitives, FU27.WASM (wasm3 vendored, `wasmd` ships, end-to-end `RUN_MODULE`). **Tag `phase-27-complete` pending user authorisation.**

**Next:**

- **Phase 28 — Final Soak + Fault Injection.** 1-hour continuous integration with random fault injection (PMM exhaustion, slab-cache failure, channel disconnect, `ahcid` death/respawn, network drops). Entry-gate requires all FU24/FU25/FU26/FU27 follow-up registries at `status: CLOSED`. Exit gate produces the `phase-28-complete` tag.

- **Real-hardware bring-up.** Sequenced *after* Phase 28 per the strategic decision in `~/.claude/plans/scalable-stirring-gem.md` — bring up once on a feature-complete OS rather than chase silicon during foundation work. ≈3–6 months focused effort, friendly-laptop target (well-documented chipset, ThinkPad/Dell-class).

- **Post-MVP research.** Phase 29+ candidates: graphical compositor, full GPU stack, distributed-channel/networked CAN, persistent capability storage across reboots, self-hosting build (compile GrahaOS on GrahaOS), formal verification of the capability core (seL4 lineage), AI-driven scheduler hints.

---

## Reading List

- **[`ROADMAP.md`](ROADMAP.md)** — phase-by-phase plan with current banner.
- **[`ACTIVATION_ARCHITECTURE.md`](ACTIVATION_ARCHITECTURE.md)** — the original CAN design document.
- **[`CLAUDE.md`](CLAUDE.md)** — development workflow, the seven design principles, current-state banner, useful commands.
- **[`specs/`](specs/)** — every Phase 12+ YAML spec; treat as primary source.
- **[`specs/MANUAL_VERIFICATION_PLAYBOOK.md`](specs/MANUAL_VERIFICATION_PLAYBOOK.md)** — aggregated end-to-end manual walkthrough.
- **[`docs/GRAHAOS-COMPREHENSIVE-REPORT.md`](docs/GRAHAOS-COMPREHENSIVE-REPORT.md)** — the ≈97k-word technical report (architecture, phase narrative, literature review, novel contributions, engineering discipline, future work, appendices).
- **[`problems/`](problems/)** — per-phase problems_faced.md with symptom / root-cause / fix / time-to-diagnose for every non-trivial bug.

Academic foundations cited heavily across the design: seL4 (Klein et al. 2009), KeyKOS (Hardy 1985), Capsicum (Watson 2010), OpenBSD pledge, Plan 9 Venti (Quinlan 2002), TxOS (Porter 2009), io_uring (Axboe 2019), WebAssembly Component Model + WASI Preview-2.

---

## Contributing

The spec-driven workflow is non-negotiable: any new feature lands as a YAML in `specs/phase-NN-codename.yml`, then implementation, then TAP tests in `user/tests/`, then `manual_verification` walkthrough, then `problems/phaseNN/problems_faced.md` writeup. The exemplar to match is [`specs/phase-12-test-harness.yml`](specs/phase-12-test-harness.yml).

Please open issues against [github.com/B-Divyesh/GrahaOS](https://github.com/B-Divyesh/GrahaOS) for design discussion before starting work.

---

## Author

GrahaOS — kernel, userspace, filesystem, network stack, capability/channel substrate, snapshot/transaction primitives, WebAssembly host, TUI framework, test harness, specs, and the comprehensive technical report at [`docs/GRAHAOS-COMPREHENSIVE-REPORT.md`](docs/GRAHAOS-COMPREHENSIVE-REPORT.md) — is designed and written by **Divyesh Bine** ([@B-Divyesh](https://github.com/B-Divyesh)).

This is a from-scratch single-author project: every line of the kernel, the userspace, and the test harness was written by hand in this tree (with the sole exception of the vendored `wasm3` interpreter under [`vendor/wasm3/`](vendor/wasm3/), MIT-licensed and used unmodified for the WebAssembly substrate). No code is forked from another OS.

---

## Final Thoughts

GrahaOS is a project of passion, vision, and experimentation. It represents a deliberate rethinking of what an operating system can be when designed *for* AI rather than *alongside* it — capability-first, manifest-driven, speculation-native, and verified end-to-end by hand and by harness.

**Contributors welcome.**
