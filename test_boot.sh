#!/bin/bash
# Test if QEMU boots properly
# This just boots and captures output for 15 seconds

echo "=== Testing QEMU Boot ===" > run.txt
echo "Date: $(date)" >> run.txt
echo "" >> run.txt

# Try to boot QEMU with output capture
# Using -serial file to capture serial output
timeout 15s qemu-system-x86_64 \
    -cdrom grahaos.iso \
    -serial file:serial_output.log \
    -m 512M \
    -smp 4 \
    -drive file=disk.img,format=raw,if=none,id=mydisk,cache=none,aio=native \
    -device ich9-ahci,id=ahci \
    -device ide-hd,drive=mydisk,bus=ahci.0 \
    -nographic \
    -d int,cpu_reset \
    -D qemu_debug.log

EXIT_CODE=$?

echo "" >> run.txt
echo "=== Boot Test Completed ===" >> run.txt
echo "Exit code: $EXIT_CODE" >> run.txt
echo "" >> run.txt

# Capture serial output
if [ -f serial_output.log ]; then
    echo "=== Serial Output ===" >> run.txt
    cat serial_output.log >> run.txt
    echo "" >> run.txt
fi

# Show results
cat run.txt
