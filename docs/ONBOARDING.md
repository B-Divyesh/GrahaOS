# GrahaOS Onboarding

A quickstart for getting GrahaOS running on a fresh clone.

## Prerequisites

- Linux host (Fedora 40 has been tested; other distros likely work).
- ~5 GB disk for the toolchain build (`toolchain/`, ~3 GB) and ISO build (`build/`, ~1 GB).
- QEMU 8.x (`qemu-system-x86_64`).
- KVM access (`/dev/kvm` readable+writable) speeds up the gate ~5x. Without it the test harness falls back to TCG (`GRAHAOS_TEST_NO_KVM=1`).

## Quick Start

```bash
git clone https://github.com/B-Divyesh/GrahaOS.git
cd GrahaOS
source ./scripts/env_setup.sh
./scripts/build-toolchain.sh   # ~30 min, one-time
make
make run        # graphical TUI in QEMU window
make terminal   # serial-only in host terminal
```

`source ./scripts/env_setup.sh` resolves the toolchain path from the script's
location and renders `scripts/cross-x86_64-elf*.ini` from the `.ini.in`
templates via `envsubst`. No hardcoded `/home/atman` paths anywhere.

## AI Agent (Optional)

GrahaOS ships a `grahai` user program that calls Gemini for plan generation.

**Status today:** AI mode is **non-functional** — the TLS stack is a stub
(`user/libtls-mg/mongoose_tls_core.c` is 634 bytes returning -ENOSYS pending
Phase 22.D.2). `http_post` over `https://` returns -EPROTO regardless of
whether you have an API key configured. The full plan execution path
(`grahai` → JSON parse → command dispatch) works on simulated/local inputs;
the live HTTPS call is the only blocker.

**Once TLS lands** (post-Phase 22.D.2):

1. Get a Gemini API key from <https://ai.google.dev/>.
2. Copy the template:
   ```bash
   cp api_keys.md.example api_keys.md
   ```
3. Edit `api_keys.md` and paste your key after `GEMINI_API_KEY=`.
4. Rebuild — the Makefile rule at `Makefile:977-984` extracts the key into
   `initrd_root/etc/ai.conf` at make time.
   ```bash
   make
   ```
5. Run GrahaOS, drop to gash, and invoke grahai:
   ```bash
   make terminal
   gash> grahai
   ```

`grahai` reads `/etc/ai.conf` at runtime, builds an HTTPS POST to
`generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=<KEY>`,
parses the JSON response, validates the resulting plan against capability
constraints, and dispatches each command.

`api_keys.md` is gitignored (`*.md` rule in `.gitignore`). Never check it in.

## Troubleshooting

### "permission denied: /dev/kvm"

The test harness needs read+write on `/dev/kvm`. On Fedora:

```bash
sudo usermod -aG kvm $USER
# log out + back in
```

Or skip KVM for one run:

```bash
GRAHAOS_TEST_NO_KVM=1 make test
```

### `make run` shows a blank QEMU window

`fbd` (the userspace framebuffer compositor) was disabled during the
pre-Phase-28 sweep because its substrate hijacks `framebuffer_draw_*` paths
without ever painting. Without `fbd`, `gash` text renders directly to the
framebuffer. If your QEMU window stays blank:

- Confirm Limine got far enough — switch to `make terminal` to see the boot
  log on serial. If serial shows boot messages, the issue is QEMU display.
- Check `/etc/init.conf` (regenerated each `make`): the line
  `daemon=bin/fbd:...` should be commented out (see `Makefile:654-666`).
- Try `-display sdl` instead of `-display gtk`: edit `Makefile:1010-1021`
  swap the flag.

### Toolchain build fails

`scripts/build-toolchain.sh` builds binutils 2.x + gcc 15.x cross-compiler
into `toolchain/`. If it fails:

- Confirm `nasm`, `make`, `gcc`, `g++`, `flex`, `bison`, `texinfo`, `xz`,
  `wget` are installed on the host.
- Check disk space (~5 GB required during build).
- The toolchain is one-time; subsequent `source scripts/env_setup.sh` calls
  re-render `.ini` files from `.ini.in` templates with the resolved path.

### Where do logs go?

- QEMU serial: stdout (under `make terminal`) or `/dev/null` (under `make run`).
- QEMU debug: `qemu.log` in repo root (cpu_reset, int events).
- Test harness: `/tmp/grahaos_tests/summary.json` + per-iter stdout.
- Boot serial under `make test`: parsed by `scripts/run_tests.sh`.

## What's Next

- See [`README.md`](../README.md) for the architecture overview and design
  principles.
- See [`docs/GRAHAOS-COMPREHENSIVE-REPORT.md`](GRAHAOS-COMPREHENSIVE-REPORT.md)
  for the full technical report.
- See [`specs/`](../specs/) for per-phase implementation specs.
- See [`CLAUDE.md`](../CLAUDE.md) for development conventions (note:
  gitignored, but checked into the repo for contributor reference).
