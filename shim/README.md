# shim — freestanding ARM64 loader flashed as `boot_a`

The phone's bootloader (ABL → EDK2) loads whatever is in `boot_a`, so instead of
an Android boot image we store a ~8 KiB program that loads a real mainline
kernel and **our** device tree. See `docs/m0-boot-shim.md` for the rationale.

## Build

```bash
./build-sky.sh                 # shim only -> out/shim.bin
./build-sky.sh payload.bin     # shim + payload -> out/sky-shim-boot-v4.img
./make-m1.sh Image.dtb         # pack Image + DTB and produce the boot image
```

Requirements: `aarch64-linux-gnu-{gcc,ld,objcopy}`, `python3`, `cpio`, and the
packers in `../tools/` (`mklinuxpayload.py`, `mkbootimg_v3.py`) — the scripts
call them relative to the repository root.

## Constraints

* Runs at an arbitrary physical address (ABL uses `0xA8000000`) with the MMU
  off → **no absolute relocations**. `build-sky.sh` refuses to build if
  `R_AARCH64_ABS64` appears in the objects; that is why the console font is a
  `char[16][16]` table instead of a pointer array.
* No libc. `console.c` writes pixels, `loader.c` copies memory.
* The kernel `Image` header's `image_size` covers `.bss`, the file does not:
  the shim zeroes that range (64-bit stores) or the kernel misbehaves.

## Files

| File | Purpose |
|------|---------|
| `start.S` | entry, stack setup, saves x0–x3, calls `shim_main()` |
| `shim_sky.c` | payload discovery (`SWEETLNX`), load addresses, `linux_boot()` |
| `loader.c` | copy helpers + `.bss` zeroing |
| `console.c`, `console.h`, `font.h` | framebuffer text console (blue band, status lines) |
| `fdt.c`, `fdt.h` | minimal FDT helpers |
| `shim.ld` | link script |
| `tools/mklinuxpayload.py` | appends the DTB to the kernel and writes the header |
| `tools/mkbootimg_v3.py` | wraps the payload into an Android v4 boot image |
| `tools/mkfakeimage.py` | tiny fake "kernel" used to test the shim alone |

## What it prints

```
SHIM     sky M1 (SM4450 ravelin)
CPU      el=… cntfrq=…
STATE    sctlr_m=… abl_dtb=…
FB       base=… stride=…
_end     …
PAYLOAD  …
RESULT   SWEETLNX found
PAYLOAD  at=… size=…
IMAGE    size=… mem=…
LOAD     pa=… flags=…
DTB      pa=… size=…
BSS      zero=… at=…
LINUX    jumping
```

`LINUX jumping` means the shim is done and control has been handed to the
kernel at EL1 with `x0 = DTB`.
