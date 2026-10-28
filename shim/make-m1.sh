#!/bin/bash
# Build the M1 boot image for sky from a real kernel Image.
#
#   ./make-m1.sh /path/to/arch/arm64/boot/Image [dtb]
#
# Defaults: DTB = ../build/sm4450-xiaomi-sky.dtb, load address = 0xc0000000
# (2 MiB aligned, inside sky's low DDR bank and above the ABL splash).
# Override the load address with LOAD=0x... if the memory map says otherwise.
set -euo pipefail
cd "$(dirname "$0")"

IMG=${1:?usage: make-m1.sh <Image> [dtb]}
DTB=${2:-../build/sm4450-xiaomi-sky.dtb}
LOAD=${LOAD:-0xc0000000}

[ -f "$IMG" ] || { echo "no such Image: $IMG" >&2; exit 1; }
[ -f "$DTB" ] || { echo "no such DTB: $DTB" >&2; exit 1; }

mkdir -p out
python3 ../tools/mklinuxpayload.py "$IMG" "$DTB" out/payload.bin --load "$LOAD"
./build-sky.sh out/payload.bin
echo
echo "flash (boot_a only, slot a):"
echo "  fastboot flash boot_a out/sky-shim-boot-v4.img && fastboot reboot"
