#!/usr/bin/env python3
import subprocess
import time
import os
import signal

print("=== GrahaOS Comprehensive Test ===\n")

# Kill any existing QEMU/GDB
os.system("pkill -9 qemu-system-x86 2>/dev/null")
os.system("pkill -9 gdb 2>/dev/null")
time.sleep(1)

# Start QEMU in background
qemu_cmd = [
    "qemu-system-x86_64",
    "-cdrom", "grahaos.iso",
    "-serial", "file:serial.log",
    "-monitor", "stdio",
    "-m", "512M",
    "-smp", "4",
    "-drive", f"file=disk.img,format=raw,if=none,id=mydisk,cache=none,aio=native",
    "-device", "ich9-ahci,id=ahci",
    "-device", "ide-hd,drive=mydisk,bus=ahci.0",
    "-s",  # GDB on :1234
    "-S",  # Start paused
]

print(f"Starting QEMU (paused for GDB)...")
qemu_proc = subprocess.Popen(qemu_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(2)

# Start GDB
print("Starting GDB...")
gdb_cmd = ["gdb", "-quiet", "-ex", "target remote localhost:1234", "-ex", "continue", "kernel.elf"]
gdb_proc = subprocess.Popen(gdb_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(3)

# Monitor serial output
print("\n=== Monitoring System Boot ===")
for i in range(30):
    time.sleep(1)
    if os.path.exists("serial.log"):
        with open("serial.log", "r") as f:
            content = f.read()
            if "magic" in content.lower():
                print(f"[{i}s] MAGIC NUMBER ISSUE DETECTED!")
            if "error" in content.lower() or "fatal" in content.lower():
                print(f"[{i}s] ERROR DETECTED!")
            if "System fully initialized" in content:
                print(f"[{i}s] System boot complete!")
                break
            if i % 5 == 0:
                print(f"[{i}s] Still booting...")

# Give it time to run
time.sleep(5)

# Check results
print("\n=== Test Results ===")
if os.path.exists("serial.log"):
    with open("serial.log", "r") as f:
        log = f.read()
        
    # Check for critical errors
    if "Invalid magic" in log:
        print("❌ FAILED: Invalid magic number error found")
    elif "FATAL" in log:
        print("❌ FAILED: Fatal error found")
    elif "System fully initialized" in log:
        print("✓ PASSED: System booted successfully")
        if "grahafs_mount returned" in log:
            print("✓ PASSED: GrahaFS mounted successfully")
        if "Shell process created" in log:
            print("✓ PASSED: User process created")
        if "SCHED] First switch to task 1" in log:
            print("✓ PASSED: User process scheduled")
    else:
        print("⚠ WARNING: Boot incomplete or hanging")

# Cleanup
qemu_proc.terminate()
gdb_proc.terminate()
time.sleep(1)
os.system("pkill -9 qemu-system-x86 2>/dev/null")
os.system("pkill -9 gdb 2>/dev/null")

print("\n=== Test Complete ===")
