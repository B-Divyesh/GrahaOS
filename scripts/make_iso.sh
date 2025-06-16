# scripts/make_iso.sh

#!/usr/bin/env bash
set -e

# 1. Prepare ISO directory
rm -rf iso
mkdir -p iso/boot/grub

# 2. Copy kernel and initramfs
cp kernel/kernel.bin iso/boot/
cp initramfs.cpio iso/boot/

# 3. Create GRUB configuration
cat > iso/boot/grub/grub.cfg << 'EOF'
set timeout=5
set default=0

menuentry "GrahaOS" {
    multiboot2 /boot/kernel.bin /boot/initramfs.cpio
    boot
}
EOF

# 4. Generate ISO with GRUB
grub-mkrescue -o GrahaOS.iso iso

echo "ISO created: GrahaOS.iso"
