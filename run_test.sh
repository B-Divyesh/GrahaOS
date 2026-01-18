#!/bin/bash
# Automated testing script

echo "=== Starting Automated QEMU Test ===" > test_run.log
echo "Date: $(date)" >> test_run.log
echo "" >> test_run.log

# Start QEMU and send commands via stdin
timeout 20s qemu-system-x86_64 \
    -cdrom grahaos.iso \
    -serial mon:stdio \
    -m 512M \
    -smp 4 \
    -drive file=disk.img,format=raw,if=none,id=mydisk,cache=none,aio=native \
    -device ich9-ahci,id=ahci \
    -device ide-hd,drive=mydisk,bus=ahci.0 \
    -d int,cpu_reset \
    -D qemu_debug.log \
    -nographic \
    <<'EOF' 2>&1 | tee -a test_run.log
sbrk_test
EOF

echo "" >> test_run.log
echo "=== Test completed ===" >> test_run.log
echo "Exit code: $?" >> test_run.log

# Check for errors
echo "=== Checking for errors ===" >> test_run.log
if grep -q "Exception\|Fault\|ERROR" test_run.log; then
    echo "ERRORS FOUND!" >> test_run.log
    grep -i "exception\|fault\|error" test_run.log >> test_run.log
else
    echo "No errors detected" >> test_run.log
fi

echo ""
echo "Results saved to test_run.log"
echo "QEMU debug log saved to qemu_debug.log"
cat test_run.log
