# scripts/build.sh

#!/usr/bin/env bash
set -e

# Load environment variables
source ./scripts/env_setup.sh

# 1. Build bootloader
echo "Building bootloader..."
cd boot
make
cd ..

# 2. Build arch/x86 code
echo "Building arch/x86..."
cd arch/x86
make CROSS_COMPILE="$TARGET-"
cd ../..

# 3. Build kernel
echo "Building kernel..."
cd kernel
make CROSS_COMPILE="$TARGET-"
cd ..

# 4. Build userland binaries
echo "Building userland..."
cd user/bin
make CROSS_COMPILE="$TARGET-"
cd ../..

# 5. Create initramfs.cpio
echo "Creating initramfs..."
./user/bin/initfs initramfs.cpio user/bin/sh user/bin/ai_client

echo "Build complete."
