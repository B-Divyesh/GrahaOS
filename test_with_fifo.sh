#!/bin/bash
# Test using FIFO for input

echo "=== QEMU Test with FIFO Input ===" > run.txt
echo "Date: $(date)" >> run.txt
echo "" >> run.txt

# Create FIFO for input
rm -f /tmp/qemu_input
mkfifo /tmp/qemu_input

# Start feeding commands to FIFO in background
(
    sleep 8  # Wait for boot
    echo "sbrk_test" > /tmp/qemu_input
    sleep 5
    echo "exit" > /tmp/qemu_input
) &
FEEDER_PID=$!

# Run QEMU with FIFO as input
timeout 20s qemu-system-x86_64 \
    -cdrom grahaos.iso \
    -serial stdio \
    -m 512M \
    -smp 4 \
    -drive file=disk.img,format=raw,if=none,id=mydisk,cache=none,aio=native \
    -device ich9-ahci,id=ahci \
    -device ide-hd,drive=mydisk,bus=ahci.0 \
    -d int,cpu_reset \
    -D qemu_debug.log \
    -nographic \
    < /tmp/qemu_input 2>&1 | tee -a run.txt

EXIT_CODE=$?

# Cleanup
kill $FEEDER_PID 2>/dev/null
rm -f /tmp/qemu_input

echo "" >> run.txt
echo "=== Test Completed ===" >> run.txt
echo "Exit code: $EXIT_CODE" >> run.txt

cat run.txt
