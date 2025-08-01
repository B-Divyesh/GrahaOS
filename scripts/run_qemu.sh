# scripts/run_qemu.sh

#!/usr/bin/env bash
set -e

if [ ! -f GrahaOS.iso ]; then
    echo "Error: GrahaOS.iso not found. Run make iso first."
    exit 1
fi

qemu-system-x86_64 \
    -m 1G \
    -cpu qemu64 \
    -drive file=GrahaOS.iso,format=raw,media=cdrom \
    -serial stdio \
    -nographic
