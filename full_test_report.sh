#!/bin/bash
echo "=== GrahaOS Full Test Report ===" 
echo "Date: $(date)"
echo ""

echo "Step 1: Checking disk.img state..."
if [ -f disk.img ]; then
    echo "  - disk.img exists ($(stat -c%s disk.img) bytes)"
    echo "  - Magic bytes: $(hexdump -C disk.img | head -1)"
else
    echo "  - disk.img MISSING!"
fi

echo ""
echo "Step 2: Re-formatting disk.img..."
./scripts/mkfs.gfs disk.img 2>&1 | grep -E "✓|Writing|Total"

echo ""
echo "Step 3: Rebuilding kernel..."
make clean >/dev/null 2>&1
make all 2>&1 | tail -5

echo ""
echo "Step 4: Running system test..."
timeout 30s python3 automated_test.py >/dev/null 2>&1

echo ""
echo "Step 5: Analyzing results..."
if [ -f serial.log ]; then
    echo "Serial log exists ($(wc -l < serial.log) lines)"
    
    if grep -q "Invalid magic" serial.log; then
        echo "  ❌ FAIL: Invalid magic number found"
        grep "magic" serial.log | head -3
    else
        echo "  ✓ PASS: No magic number errors"
    fi
    
    if grep -q "grahafs_mount returned:" serial.log; then
        echo "  ✓ PASS: GrahaFS mounted"
        grep "grahafs_mount" serial.log
    else
        echo "  ❌ FAIL: GrahaFS not mounted"
    fi
    
    if grep -q "Shell process created" serial.log; then
        echo "  ✓ PASS: User process created"
    else
        echo "  ❌ FAIL: User process not created"
    fi
    
    if grep -q "System fully initialized" serial.log; then
        echo "  ✓ PASS: System fully booted"
    else
        echo "  ❌ FAIL: System boot incomplete"
    fi
    
    if grep -q "ERROR\|FATAL\|CRASH" serial.log; then
        echo "  ❌ ERRORS FOUND:"
        grep -E "ERROR|FATAL|CRASH" serial.log | head -5
    else
        echo "  ✓ PASS: No errors detected"
    fi
else
    echo "  ❌ FAIL: No serial.log generated"
fi

echo ""
echo "=== Test Report Complete ==="
