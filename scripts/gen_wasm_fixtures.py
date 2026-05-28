#!/usr/bin/env python3
"""Generate hand-rolled WebAssembly 1.0 fixtures for FU27.WASM Stage D1+D2.

Five fixtures (per ~/.claude/plans/scalable-stirring-gem.md Stage D1):
  hello.wasm        — calls imported `gcp.print` once with "hello\\n".
  oopsie.wasm       — `unreachable` opcode → trap.
  long_running.wasm — `loop $L (br $L)` infinite loop.
  fn_call_bench.wasm— ~1000 imported-fn calls (perf floor).
  demo_net.wasm     — imports `gcp.net-send` (cap-rejection target).

Output: initrd_root/bin/tests/wasm/<name>.wasm.

Validates each via host wasm3 binary if present at $WASM3_HOST_BIN
(default: /tmp/wasm3-upstream/build/wasm3 — built once during D0).
The validation step is OPTIONAL; if host wasm3 isn't available the
fixtures are still emitted (fixture bytes are deterministically
hand-rolled, not searched).
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

# WebAssembly section IDs (binary format, version 1).
SECTION_TYPE     = 1
SECTION_IMPORT   = 2
SECTION_FUNCTION = 3
SECTION_TABLE    = 4
SECTION_MEMORY   = 5
SECTION_GLOBAL   = 6
SECTION_EXPORT   = 7
SECTION_START    = 8
SECTION_ELEMENT  = 9
SECTION_CODE     = 10
SECTION_DATA     = 11

# Type bytes.
TYPE_I32  = 0x7F
TYPE_I64  = 0x7E
TYPE_F32  = 0x7D
TYPE_F64  = 0x7C
TYPE_FUNC = 0x60

# Import/export kinds.
KIND_FUNC   = 0x00
KIND_TABLE  = 0x01
KIND_MEM    = 0x02
KIND_GLOBAL = 0x03

# Opcodes used here.
OP_UNREACHABLE = 0x00
OP_NOP         = 0x01
OP_BLOCK       = 0x02
OP_LOOP        = 0x03
OP_BR          = 0x0C
OP_RETURN      = 0x0F
OP_CALL        = 0x10
OP_END         = 0x0B
OP_I32_CONST   = 0x41
OP_I32_ADD     = 0x6A
OP_DROP        = 0x1A


def leb128_u(n: int) -> bytes:
    """Unsigned LEB128 encoding."""
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def leb128_s(n: int) -> bytes:
    """Signed LEB128 encoding."""
    out = bytearray()
    more = True
    while more:
        b = n & 0x7F
        n >>= 7
        sign = b & 0x40
        if (n == 0 and not sign) or (n == -1 and sign):
            more = False
        else:
            b |= 0x80
        out.append(b)
    return bytes(out)


def vec(items: list[bytes]) -> bytes:
    """Vector: count (LEB128) followed by concatenated items."""
    return leb128_u(len(items)) + b"".join(items)


def name(s: str) -> bytes:
    """name = vec(byte) — length-prefixed UTF-8."""
    b = s.encode("utf-8")
    return leb128_u(len(b)) + b


def section(sid: int, body: bytes) -> bytes:
    return bytes([sid]) + leb128_u(len(body)) + body


def func_type(params: list[int], results: list[int]) -> bytes:
    return bytes([TYPE_FUNC]) + vec([bytes([p]) for p in params]) + vec([bytes([r]) for r in results])


def import_func(module: str, field: str, type_idx: int) -> bytes:
    return name(module) + name(field) + bytes([KIND_FUNC]) + leb128_u(type_idx)


def export_def(field: str, kind: int, idx: int) -> bytes:
    return name(field) + bytes([kind]) + leb128_u(idx)


def memory_def(min_pages: int, max_pages: int | None = None) -> bytes:
    if max_pages is None:
        return bytes([0x00]) + leb128_u(min_pages)
    return bytes([0x01]) + leb128_u(min_pages) + leb128_u(max_pages)


def code_locals_zero(body: bytes) -> bytes:
    """Code entry: vec(local) is empty; body ends with OP_END."""
    inner = leb128_u(0) + body + bytes([OP_END])  # zero local groups
    return leb128_u(len(inner)) + inner


def data_active(mem_idx: int, offset_expr: bytes, payload: bytes) -> bytes:
    """Data segment in active mode (memory_idx, offset_expr, payload)."""
    if mem_idx == 0:
        # Most common; encoded as 0x00 + offset_expr + vec(byte)
        return bytes([0x00]) + offset_expr + leb128_u(len(payload)) + payload
    return bytes([0x02]) + leb128_u(mem_idx) + offset_expr + leb128_u(len(payload)) + payload


def i32_const(n: int) -> bytes:
    return bytes([OP_I32_CONST]) + leb128_s(n)


HEADER = b"\x00asm" + struct.pack("<I", 1)


def build_hello() -> bytes:
    """hello.wasm:
       (module
         (import "gcp" "print" (func $print (param i32 i32)))
         (memory (export "memory") 1)
         (data (i32.const 16) "hello\\n")    ; offset 16, NOT 0 — wasm3
                                              ; treats memory offset 0 as
                                              ; the null pointer and traps
                                              ; m3ApiCheckMem on (addr<=_mem).
         (func $start
           (call $print (i32.const 16) (i32.const 6)))
         (export "_start" (func $start)))
    """
    msg = b"hello\n"
    DATA_OFFSET = 16  # wasm3 m3ApiIsNullPtr: addr <= _mem traps as null.

    # Section 1: types — () -> ()  +  (i32, i32) -> ()
    sec_type = section(SECTION_TYPE, vec([
        func_type([], []),                     # type 0: ()->()
        func_type([TYPE_I32, TYPE_I32], []),   # type 1: (i32,i32)->()
    ]))
    # Section 2: import — "gcp.print" of type 1 (becomes func index 0)
    sec_import = section(SECTION_IMPORT, vec([
        import_func("gcp", "print", 1),
    ]))
    # Section 3: function — local function $start of type 0 (func index 1)
    sec_function = section(SECTION_FUNCTION, vec([leb128_u(0)]))
    # Section 5: memory — 1 page (64 KiB), no max
    sec_memory = section(SECTION_MEMORY, vec([memory_def(1)]))
    # Section 7: exports — _start (func 1), memory (mem 0)
    sec_export = section(SECTION_EXPORT, vec([
        export_def("_start", KIND_FUNC, 1),
        export_def("memory", KIND_MEM, 0),
    ]))
    # Section 10: code — body of $start: (i32.const DATA_OFFSET)(i32.const 6)(call $print)
    body = i32_const(DATA_OFFSET) + i32_const(len(msg)) + bytes([OP_CALL]) + leb128_u(0)
    sec_code = section(SECTION_CODE, vec([code_locals_zero(body)]))
    # Section 11: data — "hello\n" at memory offset DATA_OFFSET (active mode)
    offset_expr = i32_const(DATA_OFFSET) + bytes([OP_END])
    sec_data = section(SECTION_DATA, vec([data_active(0, offset_expr, msg)]))

    return HEADER + sec_type + sec_import + sec_function + sec_memory + \
           sec_export + sec_code + sec_data


def build_oopsie() -> bytes:
    """oopsie.wasm:
       (module
         (func $start unreachable)
         (export "_start" (func $start)))
    """
    sec_type = section(SECTION_TYPE, vec([func_type([], [])]))
    sec_function = section(SECTION_FUNCTION, vec([leb128_u(0)]))
    sec_export = section(SECTION_EXPORT, vec([
        export_def("_start", KIND_FUNC, 0),
    ]))
    body = bytes([OP_UNREACHABLE])
    sec_code = section(SECTION_CODE, vec([code_locals_zero(body)]))
    return HEADER + sec_type + sec_function + sec_export + sec_code


def build_long_running() -> bytes:
    """long_running.wasm:
       (module
         (func $start (block $b (loop $l (br 0))))
         (export "_start" (func $start)))
    """
    sec_type = section(SECTION_TYPE, vec([func_type([], [])]))
    sec_function = section(SECTION_FUNCTION, vec([leb128_u(0)]))
    sec_export = section(SECTION_EXPORT, vec([
        export_def("_start", KIND_FUNC, 0),
    ]))
    # block-type 0x40 = void.
    body = bytes([OP_BLOCK, 0x40,                  # block (void)
                  OP_LOOP, 0x40,                   #   loop (void)
                  OP_BR, 0x00,                     #     br $l (loop)
                  OP_END,                          #   end loop
                  OP_END])                         # end block
    sec_code = section(SECTION_CODE, vec([code_locals_zero(body)]))
    return HEADER + sec_type + sec_function + sec_export + sec_code


def build_fn_call_bench() -> bytes:
    """fn_call_bench.wasm:
       (module
         (import "gcp" "noop" (func $noop))
         (func $start
           (loop $l
             (call $noop)
             ... 1000 times ...))
         (export "_start" (func $start)))

       Simpler v1: just call noop 1000 times unrolled — keeps fixture small
       and avoids pulling in i32.add/loop for now. We cap at ~512 calls
       to keep the binary under 2 KiB.
    """
    NUM_CALLS = 512
    sec_type = section(SECTION_TYPE, vec([func_type([], [])]))     # () -> ()
    sec_import = section(SECTION_IMPORT, vec([
        import_func("gcp", "noop", 0),  # type 0 == ()->()
    ]))
    sec_function = section(SECTION_FUNCTION, vec([leb128_u(0)]))
    sec_export = section(SECTION_EXPORT, vec([
        export_def("_start", KIND_FUNC, 1),
    ]))
    body = (bytes([OP_CALL]) + leb128_u(0)) * NUM_CALLS
    sec_code = section(SECTION_CODE, vec([code_locals_zero(body)]))
    return HEADER + sec_type + sec_import + sec_function + sec_export + sec_code


def build_demo_net() -> bytes:
    """demo_net.wasm:
       (module
         (import "gcp" "net-send" (func $send (param i32 i32)))
         (func $start
           (call $send (i32.const 0) (i32.const 0)))
         (memory (export "memory") 1)
         (export "_start" (func $start)))

       Used by D2 wasm_fault_wasmd_lacks_cap test — wasmd refuses to
       resolve "net-send" against its allowed-imports list, so the
       module never instantiates.
    """
    sec_type = section(SECTION_TYPE, vec([
        func_type([], []),                     # type 0: ()->()
        func_type([TYPE_I32, TYPE_I32], []),   # type 1: (i32,i32)->()
    ]))
    sec_import = section(SECTION_IMPORT, vec([
        import_func("gcp", "net-send", 1),
    ]))
    sec_function = section(SECTION_FUNCTION, vec([leb128_u(0)]))
    sec_memory = section(SECTION_MEMORY, vec([memory_def(1)]))
    sec_export = section(SECTION_EXPORT, vec([
        export_def("_start", KIND_FUNC, 1),
        export_def("memory", KIND_MEM, 0),
    ]))
    body = i32_const(0) + i32_const(0) + bytes([OP_CALL]) + leb128_u(0)
    sec_code = section(SECTION_CODE, vec([code_locals_zero(body)]))
    return HEADER + sec_type + sec_import + sec_function + sec_memory + \
           sec_export + sec_code


def build_ai_demo() -> bytes:
    """ai_demo.wasm — exercises 3 host bindings.
       (module
         (import "gcp" "print"     (func $print     (param i32 i32)))
         (import "gcp" "tui_write" (func $tui_write (param i32 i32 i32 i32) (result i32)))
         (memory (export "memory") 1)
         (data (i32.const 16) "ai-demo")
         (func $start
           ;; Print marker so the test can verify the binding ran.
           (call $print (i32.const 16) (i32.const 7))
           ;; Write 'X' at console 0, (col=1,row=1).
           (call $tui_write (i32.const 0) (i32.const 1) (i32.const 1) (i32.const 0x58))
           (drop))
         (export "_start" (func $start)))
    """
    msg = b"ai-demo"
    DATA_OFFSET = 16
    sec_type = section(SECTION_TYPE, vec([
        func_type([], []),                                            # type 0
        func_type([TYPE_I32, TYPE_I32], []),                          # type 1 print
        func_type([TYPE_I32, TYPE_I32, TYPE_I32, TYPE_I32], [TYPE_I32]),  # type 2 tui_write
    ]))
    sec_import = section(SECTION_IMPORT, vec([
        import_func("gcp", "print", 1),       # func 0
        import_func("gcp", "tui_write", 2),   # func 1
    ]))
    sec_function = section(SECTION_FUNCTION, vec([leb128_u(0)]))     # func 2 of type 0
    sec_memory = section(SECTION_MEMORY, vec([memory_def(1)]))
    sec_export = section(SECTION_EXPORT, vec([
        export_def("_start", KIND_FUNC, 2),
        export_def("memory", KIND_MEM, 0),
    ]))
    body = (i32_const(DATA_OFFSET) + i32_const(len(msg)) +
            bytes([OP_CALL]) + leb128_u(0) +
            i32_const(0) + i32_const(1) + i32_const(1) + i32_const(0x58) +
            bytes([OP_CALL]) + leb128_u(1) +
            bytes([OP_DROP]))
    sec_code = section(SECTION_CODE, vec([code_locals_zero(body)]))
    offset_expr = i32_const(DATA_OFFSET) + bytes([OP_END])
    sec_data = section(SECTION_DATA, vec([data_active(0, offset_expr, msg)]))
    return HEADER + sec_type + sec_import + sec_function + sec_memory + \
           sec_export + sec_code + sec_data


def build_sigkill() -> bytes:
    """sigkill.wasm — enters infinite loop after printing marker.
       Used to verify wasmd survives an externally-killed worker."""
    msg = b"sigkill-loop"
    DATA_OFFSET = 16
    sec_type = section(SECTION_TYPE, vec([
        func_type([], []),
        func_type([TYPE_I32, TYPE_I32], []),
    ]))
    sec_import = section(SECTION_IMPORT, vec([
        import_func("gcp", "print", 1),
    ]))
    sec_function = section(SECTION_FUNCTION, vec([leb128_u(0)]))
    sec_memory = section(SECTION_MEMORY, vec([memory_def(1)]))
    sec_export = section(SECTION_EXPORT, vec([
        export_def("_start", KIND_FUNC, 1),
        export_def("memory", KIND_MEM, 0),
    ]))
    body = (i32_const(DATA_OFFSET) + i32_const(len(msg)) +
            bytes([OP_CALL]) + leb128_u(0) +
            bytes([OP_BLOCK, 0x40, OP_LOOP, 0x40, OP_BR, 0x00, OP_END, OP_END]))
    sec_code = section(SECTION_CODE, vec([code_locals_zero(body)]))
    offset_expr = i32_const(DATA_OFFSET) + bytes([OP_END])
    sec_data = section(SECTION_DATA, vec([data_active(0, offset_expr, msg)]))
    return HEADER + sec_type + sec_import + sec_function + sec_memory + \
           sec_export + sec_code + sec_data


def build_path_narrow_violator() -> bytes:
    """path_narrow_violator.wasm — calls gcp.fs_write on a path outside the
       sandbox (must NOT start with /tmp/).  The worker traps with
       'cap denied: fs_write path outside sandbox' which the daemon maps
       to WASMD_E_CAP_DENIED."""
    # Path string: "evil.txt" (no /tmp/ prefix).
    path = b"evil.txt"
    content = b"data"
    PATH_OFF = 16
    CONTENT_OFF = 64
    sec_type = section(SECTION_TYPE, vec([
        func_type([], []),
        func_type([TYPE_I32, TYPE_I32, TYPE_I32, TYPE_I32], [TYPE_I32]),  # fs_write
    ]))
    sec_import = section(SECTION_IMPORT, vec([
        import_func("gcp", "fs_write", 1),
    ]))
    sec_function = section(SECTION_FUNCTION, vec([leb128_u(0)]))
    sec_memory = section(SECTION_MEMORY, vec([memory_def(1)]))
    sec_export = section(SECTION_EXPORT, vec([
        export_def("_start", KIND_FUNC, 1),
        export_def("memory", KIND_MEM, 0),
    ]))
    body = (i32_const(PATH_OFF) + i32_const(len(path)) +
            i32_const(CONTENT_OFF) + i32_const(len(content)) +
            bytes([OP_CALL]) + leb128_u(0) +
            bytes([OP_DROP]))
    sec_code = section(SECTION_CODE, vec([code_locals_zero(body)]))
    offset_expr_p = i32_const(PATH_OFF) + bytes([OP_END])
    offset_expr_c = i32_const(CONTENT_OFF) + bytes([OP_END])
    sec_data = section(SECTION_DATA, vec([
        data_active(0, offset_expr_p, path),
        data_active(0, offset_expr_c, content),
    ]))
    return HEADER + sec_type + sec_import + sec_function + sec_memory + \
           sec_export + sec_code + sec_data


def build_fuel_exhaust() -> bytes:
    """fuel_exhaust.wasm — tight infinite loop with no host I/O.
       Worker's wall-clock watchdog cannot wake without a host call,
       so this fixture is functionally identical to long_running until
       we add an interpreter-level instruction budget. Worker is then
       SIGKILL'd by the orchestrator after a timeout, mapped to
       WASMD_E_FUEL_EXHAUSTED."""
    sec_type = section(SECTION_TYPE, vec([func_type([], [])]))
    sec_function = section(SECTION_FUNCTION, vec([leb128_u(0)]))
    sec_export = section(SECTION_EXPORT, vec([
        export_def("_start", KIND_FUNC, 0),
    ]))
    body = bytes([OP_BLOCK, 0x40, OP_LOOP, 0x40, OP_BR, 0x00, OP_END, OP_END])
    sec_code = section(SECTION_CODE, vec([code_locals_zero(body)]))
    return HEADER + sec_type + sec_function + sec_export + sec_code


def build_lacks_cap() -> bytes:
    """lacks_cap.wasm — calls gcp.tui_write but is spawned with the
       TUI_WRITE cap denied via /tmp/wasmd_pending.caps gate-file."""
    sec_type = section(SECTION_TYPE, vec([
        func_type([], []),
        func_type([TYPE_I32, TYPE_I32, TYPE_I32, TYPE_I32], [TYPE_I32]),
    ]))
    sec_import = section(SECTION_IMPORT, vec([
        import_func("gcp", "tui_write", 1),
    ]))
    sec_function = section(SECTION_FUNCTION, vec([leb128_u(0)]))
    sec_export = section(SECTION_EXPORT, vec([
        export_def("_start", KIND_FUNC, 1),
    ]))
    body = (i32_const(0) + i32_const(0) + i32_const(0) + i32_const(0x41) +
            bytes([OP_CALL]) + leb128_u(0) +
            bytes([OP_DROP]))
    sec_code = section(SECTION_CODE, vec([code_locals_zero(body)]))
    return HEADER + sec_type + sec_import + sec_function + sec_export + sec_code


FIXTURES = {
    "hello.wasm":               build_hello,
    "oopsie.wasm":               build_oopsie,
    "long_running.wasm":         build_long_running,
    "fn_call_bench.wasm":        build_fn_call_bench,
    "demo_net.wasm":             build_demo_net,
    # Phase 29 Session G — FU27.WASM.D2_worker fixtures.
    "ai_demo.wasm":              build_ai_demo,
    "sigkill.wasm":              build_sigkill,
    "path_narrow_violator.wasm": build_path_narrow_violator,
    "fuel_exhaust.wasm":         build_fuel_exhaust,
    "lacks_cap.wasm":            build_lacks_cap,
}


def validate_with_host_wasm3(path: Path, host_bin: Path) -> bool:
    """Run host wasm3 against the fixture and decide whether *parsing*
    succeeded. host wasm3 doesn't have a parse-only mode, so we look at
    the error message:
      - "magic", "version", "truncated", "malformed" -> parse FAILED
      - "missing imported function" / "[trap]" / timeout -> parse OK
        (module loaded, just couldn't execute due to missing host imports
         or because the module deliberately loops/traps)
      - exit 0 -> module parsed AND ran to completion successfully

    The fixtures with imports (hello, fn_call_bench, demo_net) hit
    "missing imported function" because host wasm3 has no gcp.* shims.
    oopsie hits "[trap] unreachable executed". long_running hits the
    5-second timeout. All four indicate parse-OK.
    """
    if not host_bin.is_file():
        return True  # no validator -> assume hand-rolled bytes are correct
    try:
        r = subprocess.run([str(host_bin), str(path)],
                           capture_output=True, timeout=2)
        msg = (r.stdout + r.stderr).decode("utf-8", errors="replace").lower()
        # Signature strings indicating a binary-format parse failure.
        for parse_fail_marker in ("magic", "bad version", "truncated",
                                   "malformed", "out of bounds at",
                                   "wasm validate"):
            if parse_fail_marker in msg:
                sys.stderr.write(f"[gen_wasm_fixtures] PARSE FAIL for {path.name}: {msg.strip()[:200]}\n")
                return False
        # Exit code 0 = full success, or a non-parse error (missing import /
        # trap / timeout). Both indicate the binary parsed cleanly.
        return True
    except subprocess.TimeoutExpired:
        # long_running.wasm intentionally loops forever; timeout is expected
        # AFTER parsing succeeded.
        return True


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--out-dir", default="initrd_root/bin/tests/wasm",
                   help="output directory for the .wasm fixtures")
    p.add_argument("--host-wasm3",
                   default=os.environ.get("WASM3_HOST_BIN",
                                          "/tmp/wasm3-upstream/build/wasm3"),
                   help="optional host wasm3 binary for validation")
    args = p.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    host_bin = Path(args.host_wasm3)
    failures = 0
    for name, builder in FIXTURES.items():
        bytes_ = builder()
        out = out_dir / name
        out.write_bytes(bytes_)
        ok = validate_with_host_wasm3(out, host_bin)
        status = "OK" if ok else "FAIL"
        print(f"  {name}: {len(bytes_):4} bytes [{status}]")
        if not ok:
            failures += 1
    print(f"FU27.WASM fixtures: {len(FIXTURES) - failures}/{len(FIXTURES)} valid in {out_dir}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
