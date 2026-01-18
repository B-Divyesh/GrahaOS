#!/usr/bin/env python3
import subprocess
import time
import os

print("=== Testing with VNC to see actual output ===\n")

# Cleanup
os.system("pkill -9 qemu-system-x86 2>/dev/null")
time.sleep(1)

# Run QEMU with VNC for 20 seconds
qemu_cmd = [
    "qemu-system-x86_64",
    "-cdrom", "grahaos.iso",
    "-serial", "file:serial.log",
    "-m", "512M",
    "-smp", "4",
    "-drive", "file=disk.img,format=raw,if=none,id=mydisk",
    "-device", "ich9-ahci,id=ahci",
    "-device", "ide-hd,drive=mydisk,bus=ahci.0",
    "-vnc", ":0",
    "-monitor", "none"
]

print("Starting QEMU with VNC on :5900")
print("Connect with: vncviewer localhost:5900")
print("Or check serial.log for output")
print("\nRunning for 20 seconds...\n")

proc = subprocess.Popen(qemu_cmd)
time.sleep(20)

# Show last part of serial log
print("=== Serial Log (last 100 lines) ===")
if os.path.exists("serial.log"):
    os.system("tail -100 serial.log")

proc.terminate()
time.sleep(1)
os.system("pkill -9 qemu-system-x86 2>/dev/null")
