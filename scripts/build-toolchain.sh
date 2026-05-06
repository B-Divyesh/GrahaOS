#!/usr/bin/env bash
# scripts/build-toolchain.sh — convenience wrapper to build the
# x86_64-elf cross-compiler under <repo>/toolchain.
#
# Usage:
#   ./scripts/build-toolchain.sh           # build into <repo>/toolchain
#   TOOLCHAIN_PREFIX=/opt/toolchain ./scripts/build-toolchain.sh
#
# This is one-time work (~30 min) on a fresh clone. The Makefile expects
# x86_64-elf-gcc, -ld, -ar, -strip, -objcopy in $PREFIX/bin. After this
# script finishes, source scripts/env_setup.sh to add $PREFIX/bin to PATH
# and render the cross-compile .ini files from their .ini.in templates.

set -e

# Resolve repo root from this script's location.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PREFIX="${TOOLCHAIN_PREFIX:-${REPO_ROOT}/toolchain}"
TARGET="x86_64-elf"
BINUTILS_VER="${BINUTILS_VER:-2.42}"
GCC_VER="${GCC_VER:-15.1.0}"

# If toolchain already exists with a working gcc, skip.
if [ -x "${PREFIX}/bin/${TARGET}-gcc" ]; then
    echo "Toolchain already present at ${PREFIX} — skipping build."
    echo "  Detected: $(${PREFIX}/bin/${TARGET}-gcc --version | head -1)"
    echo "Tip: source ./scripts/env_setup.sh to render the .ini files."
    exit 0
fi

# Pre-flight: required host tools.
need_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "ERROR: missing host tool: $1" >&2
        echo "  Install via your distro package manager." >&2
        exit 1
    }
}
for cmd in nasm make gcc g++ flex bison makeinfo wget tar xz; do
    need_cmd "$cmd"
done

WORK="${REPO_ROOT}/.toolchain-build"
mkdir -p "$WORK" "$PREFIX"
cd "$WORK"

# binutils.
if [ ! -d "binutils-${BINUTILS_VER}" ]; then
    echo "Fetching binutils-${BINUTILS_VER}..."
    wget -q "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
    tar -xf "binutils-${BINUTILS_VER}.tar.xz"
fi
mkdir -p "binutils-build"
cd binutils-build
if [ ! -f .configured ]; then
    "../binutils-${BINUTILS_VER}/configure" \
        --target="$TARGET" --prefix="$PREFIX" \
        --with-sysroot --disable-nls --disable-werror \
        --enable-gold
    touch .configured
fi
make -j"$(nproc)"
make install
cd ..

# gcc (only stage 1 + libgcc).
if [ ! -d "gcc-${GCC_VER}" ]; then
    echo "Fetching gcc-${GCC_VER}..."
    wget -q "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"
    tar -xf "gcc-${GCC_VER}.tar.xz"
    cd "gcc-${GCC_VER}"
    ./contrib/download_prerequisites
    cd ..
fi
mkdir -p "gcc-build"
cd gcc-build
if [ ! -f .configured ]; then
    "../gcc-${GCC_VER}/configure" \
        --target="$TARGET" --prefix="$PREFIX" \
        --disable-nls --enable-languages=c,c++ --without-headers \
        --disable-libssp --disable-libquadmath
    touch .configured
fi
make -j"$(nproc)" all-gcc
make install-gcc
make -j"$(nproc)" all-target-libgcc
make install-target-libgcc
cd ..

# Done.
echo ""
echo "Toolchain built at: ${PREFIX}"
echo "Binaries: ${PREFIX}/bin/${TARGET}-{gcc,g++,ld,ar,strip,objcopy}"
echo ""
echo "Next:"
echo "  source ./scripts/env_setup.sh   # adds toolchain/bin to PATH"
echo "  make                            # build kernel + userspace + ISO"
echo "  make run                        # boot in graphical QEMU"
echo "  make terminal                   # boot in host terminal (serial)"
