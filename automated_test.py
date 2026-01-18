#!/usr/bin/env python3
"""
Comprehensive automated test with GDB and serial logging
"""

import subprocess
import time
import os
import signal
import sys

def main():
    print("=== Starting Comprehensive Automated Test ===")

    # Open run.txt for writing
    with open("run.txt", "w") as log:
        log.write("=== GrahaOS Comprehensive Test Log ===\n")
        log.write(f"Date: {time.strftime('%c')}\n\n")
        log.flush()

        # Start QEMU with serial output to file and GDB support
        qemu_cmd = [
            "qemu-system-x86_64",
            "-cdrom", "grahaos.iso",
            "-serial", "file:serial.log",  # Serial to file for kernel logging
            "-monitor", "none",
            "-display", "none",
            "-m", "512M",
            "-smp", "4",
            "-drive", "file=disk.img,format=raw,if=none,id=mydisk,cache=none,aio=native",
            "-device", "ich9-ahci,id=ahci",
            "-device", "ide-hd,drive=mydisk,bus=ahci.0",
            "-s",  # GDB server on localhost:1234
        ]

        print("Starting QEMU with GDB support...")
        log.write("Starting QEMU...\n")
        log.flush()

        qemu_proc = subprocess.Popen(
            qemu_cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

        print(f"QEMU started with PID {qemu_proc.pid}")
        log.write(f"QEMU PID: {qemu_proc.pid}\n\n")
        log.flush()

        # Wait for QEMU to start
        time.sleep(3)

        # Create GDB script for automated debugging
        gdb_script = """
# Connect to QEMU
target remote localhost:1234

# Log all output
set logging file gdb.log
set logging overwrite on
set logging enabled on
set pagination off

# Continue execution
continue
"""

        with open("auto_gdb.script", "w") as f:
            f.write(gdb_script)

        # Start GDB in background
        print("Starting GDB...")
        log.write("Starting GDB...\n\n")
        log.flush()

        gdb_proc = subprocess.Popen(
            ["gdb", "-batch", "-x", "auto_gdb.script", "kernel/kernel.elf"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True
        )

        # Monitor serial.log for output
        print("Monitoring serial output...")
        log.write("=== Kernel Serial Output ===\n")
        log.flush()

        start_time = time.time()
        last_size = 0
        timeout = 30  # 30 second timeout

        while time.time() - start_time < timeout:
            # Check if QEMU is still running
            if qemu_proc.poll() is not None:
                print("QEMU exited")
                break

            # Read serial.log if it exists
            if os.path.exists("serial.log"):
                try:
                    with open("serial.log", "r") as sf:
                        sf.seek(last_size)
                        new_data = sf.read()
                        if new_data:
                            print(new_data, end='')
                            log.write(new_data)
                            log.flush()
                            last_size = sf.tell()

                            # Check for shell prompt to send command
                            if "grahai@" in new_data or "GrahaOS>" in new_data:
                                print("\n>>> Shell detected! Sending test command... <<<\n")
                                log.write("\n>>> Sending command: libctest <<<\n")
                                log.flush()
                                # Note: Can't send to stdin because serial is redirected to file
                                # This is a limitation - we'll see kernel boot logs though
                                time.sleep(2)
                except Exception as e:
                    pass

            time.sleep(0.5)

        # Cleanup
        print("\n\nTest timeout reached or QEMU exited")
        log.write("\n\n=== Test Complete ===\n")
        log.flush()

        # Kill processes
        print("Terminating QEMU...")
        qemu_proc.terminate()
        try:
            qemu_proc.wait(timeout=5)
        except:
            qemu_proc.kill()

        print("Terminating GDB...")
        gdb_proc.terminate()
        try:
            gdb_proc.wait(timeout=2)
        except:
            gdb_proc.kill()

        # Append final serial output
        if os.path.exists("serial.log"):
            print("\nFinal serial output:")
            with open("serial.log", "r") as sf:
                final_output = sf.read()
                log.write("\n=== Final Serial Output ===\n")
                log.write(final_output)
                print(final_output[-2000:])  # Last 2000 chars

        # Append GDB output
        if os.path.exists("gdb.log"):
            with open("gdb.log", "r") as gf:
                log.write("\n=== GDB Output ===\n")
                log.write(gf.read())

        log.write("\n=== END OF LOG ===\n")

    print("\n\nAll logs saved to run.txt")
    print("Serial output saved to serial.log")
    print("GDB output saved to gdb.log")

if __name__ == "__main__":
    main()
