# Top-level Meson build file for GrahaOS.
#
# Phase 12: bootstrap shim. Delegates build and test to the existing
# Makefile and scripts/run_tests.sh. A full Meson port of the ~43
# kernel source files plus user binaries plus Mongoose's SSE exception
# is deferred to Phase 13 per risk #5 in the authoritative plan at
# ~/.claude/plans/scalable-stirring-gem.md — this shim is the minimum
# that lets downstream tooling invoke `meson compile` and `meson test`.
#
# Invocation:
#   meson setup build --cross-file=scripts/cross-x86_64-elf.ini
#   meson compile -C build            # produces grahaos.iso
#   meson test -C build --print-errorlogs
#
# For day-to-day work, the Makefile is still primary: `make`, `make
# test`, `make test-sentinel-meta`, `make qemu-interactive`.

project(
    'grahaos',
    version: '0.12.0',
    meson_version: '>=1.3',
    default_options: [
        'warning_level=2',
        'werror=false',
        'buildtype=debugoptimized',
    ],
)

# --- Host tools used by the build ---
tar_prog     = find_program('tar',     required: true)
xorriso_prog = find_program('xorriso', required: true)
dd_prog      = find_program('dd',      required: true)

# --- Build options ---
with_sentinel_fail = get_option('with_sentinel_fail')

# --- Build target: grahaos.iso ---
# Delegates to the Makefile, which is the authoritative build driver
# for Phase 12. Work unit 2 (full Meson port of kernel+arch+user) is
# deferred to Phase 13; this shim ensures any Meson-aware tooling
# sees a correct build target today.
grahaos_iso = custom_target(
    'grahaos-iso',
    output: 'grahaos.iso',
    command: [
        'sh', '-c',
        'cd "@SOURCE_ROOT@" && make -s all >&2 && cp grahaos.iso "@OUTPUT@"',
    ],
    build_by_default: true,
    console: true,
)

# --- Test suite wiring ---
# `meson test -C build` delegates to scripts/run_tests.sh which is the
# same entry point `make test` uses. Single source of truth.
test(
    'grahaos-harness',
    find_program('sh'),
    args: [
        '-c',
        'cd "@0@" && scripts/run_tests.sh'.format(meson.project_source_root()),
    ],
    workdir: meson.project_source_root(),
    timeout: 180,
    is_parallel: false,
    priority: 0,
)

# Meta-test: `meson test -C build --suite=meta` proves the harness
# catches failures. Wraps `make test-sentinel-meta` which should exit
# 0 (the wrapper inverts the underlying run_tests.sh non-zero exit).
test(
    'grahaos-harness-meta',
    find_program('sh'),
    args: [
        '-c',
        'cd "@0@" && make test-sentinel-meta'.format(meson.project_source_root()),
    ],
    workdir: meson.project_source_root(),
    timeout: 180,
    is_parallel: false,
    priority: 1,
    suite: 'meta',
)

summary(
    {
        'build-system': 'Meson ' + meson.version() + ' (shim delegating to Makefile)',
        'cross-file':   meson.is_cross_build() ? 'yes' : 'no',
        'with_sentinel_fail': with_sentinel_fail,
    },
    section: 'Phase 12 bootstrap',
)
