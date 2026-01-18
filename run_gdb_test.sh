#!/bin/bash
# Script to run QEMU with GDB and trace execution

echo "=== GDB Debugging Session ===" > run.txt
echo "Date: $(date)" >> run.txt
echo "" >> run.txt

# Start QEMU in background with GDB support
qemu-system-x86_64 \
    -cdrom grahaos.iso \
    -serial stdio \
    -monitor none \
    -nographic \
    -m 512M \
    -smp 4 \
    -drive file=disk.img,format=raw,if=none,id=mydisk,cache=none,aio=native \
    -device ich9-ahci,id=ahci \
    -device ide-hd,drive=mydisk,bus=ahci.0 \
    -s -S \
    > qemu_serial.log 2>&1 &

QEMU_PID=$!
echo "QEMU started with PID $QEMU_PID" | tee -a run.txt

# Wait for QEMU to be ready
sleep 2

# Run GDB in batch mode
echo "Starting GDB..." | tee -a run.txt
timeout 30s gdb -batch -x debug_sbrk.gdb kernel/kernel.elf >> run.txt 2>&1

# Kill QEMU
echo "" | tee -a run.txt
echo "Terminating QEMU..." | tee -a run.txt
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null

# Append GDB log
echo "" >> run.txt
echo "=== GDB Trace Log ===" >> run.txt
if [ -f gdb_trace.log ]; then
    cat gdb_trace.log >> run.txt
fi

# Append QEMU serial log
echo "" >> run.txt
echo "=== QEMU Serial Output ===" >> run.txt
if [ -f qemu_serial.log ]; then
    cat qemu_serial.log >> run.txt
fi

echo "" >> run.txt
echo "=== Test Complete ===" >> run.txt

echo ""
echo "Results saved to run.txt"
echo "Last 100 lines:"
tail -100 run.txt
