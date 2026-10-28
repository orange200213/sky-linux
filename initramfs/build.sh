#!/bin/bash
set -e
cd "$(dirname "$0")"
find . -print0 | cpio --null -o -H newc -R 0:0 2>/dev/null | gzip -9 > sky-m2-initramfs.cpio.gz
ls -la sky-m2-initramfs.cpio.gz
