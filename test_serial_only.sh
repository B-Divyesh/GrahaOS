#!/bin/bash
# Test QEMU boot with serial output only

echo "=== QEMU Serial Boot Test ===" > run.txt
echo "Date: $(date)" >> run.txt
echo "" >> run.txt

# Run QEMU with display disabled but serial enabled
# This should let Limine boot properly while capturing all output
timeout 30s qemu-system-x86_64 \
    -cdrom grahaos.iso \
    -serial stdio \
    -display none \
    -m 512M \
    -smp 4 \
    -drive file=disk.img,format=raw,if=none,id=mydisk,cache=none,aio=native \
    -device ich9-ahci,id=ahci \
    -device ide-hd,drive=mydisk,bus=ahci.0 \
    -d int,cpu_reset \
    -D qemu_debug.log 2>&1 | tee -a run.txt

EXIT_CODE=$?

echo "" >> run.txt
echo "=== Test Completed ===" >> run.txt
echo "Exit code: $EXIT_CODE" >> run.txt
echo "" >> run.txt

# Check for errors
echo "=== Checking for Errors ===" >> run.txt
if grep -qi "exception\|fault\|error\|panic" run.txt; then
    echo "ERRORS FOUND!" >> run.txt
    grep -i "exception\|fault\|error\|panic" run.txt | head -20 >> run.txt
else
    echo "No critical errors detected in output" >> run.txt
fi

echo ""
echo "Full log saved to run.txt"
echo "Last 50 lines of output:"
tail -50 run.txt
