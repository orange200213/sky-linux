#!/bin/bash
# Build the sky M1 shim and wrap it into a boot image for boot_a.
#
#   ./build-sky.sh                 # shim + 4K test blob (no payload)
#   ./build-sky.sh payload.bin     # shim + SWEETLNX payload (Image+DTB)
#
# Output: out/sky-shim-boot-v4.img (header v4, what stock sky uses) and
#         out/sky-shim-boot-v3.img (fallback).
# Framebuffer geometry defaults to 0xb8000000 / 1080x2460, override with
#   SHIM_FB_BASE=0x... SHIM_FB_W=... SHIM_FB_H=... ./build-sky.sh
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-aarch64-linux-gnu-gcc}
OBJCOPY=${OBJCOPY:-aarch64-linux-gnu-objcopy}
NM=${NM:-aarch64-linux-gnu-nm}
READELF=${READELF:-aarch64-linux-gnu-readelf}
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
CFLAGS="$CFLAGS -DSHIM_FB_BASE=${SHIM_FB_BASE:-0xb8000000} -DSHIM_FB_W=${SHIM_FB_W:-1080} -DSHIM_FB_H=${SHIM_FB_H:-2460}"

SRCS="start.S shim_sky.c console.c loader.c"
mkdir -p out
for f in $SRCS; do
	$CC $CFLAGS -c "$f" -o "out/${f%.*}.o"
done

# The image runs wherever ABL put it, with no relocation pass: no absolute
# relocations allowed in the C objects (only the header's image_size may be).
abs=$($READELF -r out/shim_sky.o out/console.o out/loader.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }

$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim_sky.o out/console.o out/loader.o -o out/shim.elf

$OBJCOPY -O binary out/shim.elf out/Image

# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$((16#$($NM out/shim.elf | awk '$3 == "_end" { print $1 }')))
truncate -s "$size" out/Image

# Embedded blob at the first 4K boundary after _end, then grow image_size.
truncate -s $(( (size + 0xfff) & ~0xfff )) out/Image
if [ -n "${1:-}" ]; then
	cat "$1" >> out/Image
else
	python3 -c 'import sys; sys.stdout.buffer.write(b"SKYEMBED".ljust(4096, b"\0"))' >> out/Image
fi
python3 -c 'import os,struct,sys; f=open(sys.argv[1],"r+b"); f.seek(16); f.write(struct.pack("<Q", os.path.getsize(sys.argv[1])))' out/Image

# Ramdisk: uncompressed newc cpio holding the marker the shim searches for.
rm -rf out/rd && mkdir -p out/rd
printf 'SKY-RD-MARK-1\n' > out/rd/sky-marker
(cd out/rd && find . | LC_ALL=C sort | cpio -o -H newc --quiet --reproducible > ../ramdisk.cpio 2>/dev/null \
	|| (cd out/rd && find . -exec touch -d @0 {} + && find . | LC_ALL=C sort | cpio -o -H newc --quiet > ../ramdisk.cpio))

python3 ../tools/mkbootimg_v3.py --header-version 4 --kernel out/Image --ramdisk out/ramdisk.cpio \
	--cmdline "console=tty0" -o out/sky-shim-boot-v4.img
python3 ../tools/mkbootimg_v3.py --header-version 3 --kernel out/Image --ramdisk out/ramdisk.cpio \
	--cmdline "console=tty0" -o out/sky-shim-boot-v3.img
ls -l out/Image out/sky-shim-boot-v4.img out/sky-shim-boot-v3.img
sha256sum out/sky-shim-boot-v4.img out/sky-shim-boot-v3.img
