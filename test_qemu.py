#!/usr/bin/env python3
"""
Automated QEMU testing script for GrahaOS
"""

import subprocess
import time
import sys
import signal
import os

def run_qemu_test():
    print("=== Starting Automated QEMU Test ===")
    print(f"Date: {time.strftime('%c')}")
    print()

    # Open log file
    log = open("run.txt", "w")
    log.write("=== GrahaOS Automated Test Log ===\n")
    log.write(f"Date: {time.strftime('%c')}\n\n")
    log.flush()

    # QEMU command
    qemu_cmd = [
        "qemu-system-x86_64",
        "-cdrom", "grahaos.iso",
        "-serial", "stdio",
        "-monitor", "none",  # Disable monitor to avoid conflict with -serial stdio
        "-nographic",
        "-m", "512M",
        "-smp", "4",
        "-drive", "file=disk.img,format=raw,if=none,id=mydisk,cache=none,aio=native",
        "-device", "ich9-ahci,id=ahci",
        "-device", "ide-hd,drive=mydisk,bus=ahci.0",
        "-d", "int,cpu_reset",
        "-D", "qemu_debug.log",
    ]

    print("Starting QEMU...")
    log.write("Starting QEMU...\n")
    log.flush()

    try:
        # Start QEMU process
        proc = subprocess.Popen(
            qemu_cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1
        )

        # Wait for boot (monitor output)
        boot_timeout = 25  # Increased timeout for slow boot with -nographic
        start_time = time.time()
        output_buffer = []
        shell_ready = False

        print("Waiting for system to boot (this may take up to 25 seconds)...")
        log.write("Waiting for system to boot (this may take up to 25 seconds)...\n")
        log.flush()

        while time.time() - start_time < boot_timeout:
            # Check if process is still running
            if proc.poll() is not None:
                print("QEMU exited unexpectedly!")
                log.write("QEMU exited unexpectedly!\n")
                break

            # Read output with timeout
            try:
                line = proc.stdout.readline()
                if line:
                    output_buffer.append(line)
                    print(line, end='')
                    log.write(line)
                    log.flush()

                    # Check for shell prompt
                    if "grahai@" in line or "GrahaOS>" in line or "$" in line.strip():
                        shell_ready = True
                        print("\n>>> Shell prompt detected! <<<\n")
                        log.write("\n>>> Shell prompt detected! <<<\n")
                        log.flush()
                        break
            except:
                time.sleep(0.1)
                continue

        if shell_ready:
            print("Sending test command: sbrk_test")
            log.write("\nSending command: sbrk_test\n")
            log.flush()

            # Send command
            proc.stdin.write("sbrk_test\n")
            proc.stdin.flush()

            # Wait for test output
            test_timeout = 5
            start_time = time.time()

            while time.time() - start_time < test_timeout:
                if proc.poll() is not None:
                    break

                try:
                    line = proc.stdout.readline()
                    if line:
                        print(line, end='')
                        log.write(line)
                        log.flush()

                        # Check for test completion
                        if "All tests passed" in line or "test complete" in line.lower():
                            print("\n>>> Test completed successfully! <<<\n")
                            log.write("\n>>> Test completed successfully! <<<\n")
                            break
                        elif "exception" in line.lower() or "fault" in line.lower():
                            print(f"\n>>> ERROR DETECTED: {line} <<<\n")
                            log.write(f"\n>>> ERROR DETECTED: {line} <<<\n")
                            break
                except:
                    time.sleep(0.1)
                    continue
        else:
            print("Shell prompt not detected within timeout")
            log.write("Shell prompt not detected within timeout\n")

        # Cleanup
        print("\nTerminating QEMU...")
        log.write("\nTerminating QEMU...\n")

        try:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=2)
        except:
            proc.kill()
            proc.wait()

    except Exception as e:
        print(f"Error: {e}")
        log.write(f"Error: {e}\n")
    finally:
        log.write("\n=== Test Complete ===\n")
        log.close()
        print("\nLog saved to run.txt")

if __name__ == "__main__":
    run_qemu_test()
