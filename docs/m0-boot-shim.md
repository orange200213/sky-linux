# M0 — boot shim

## Why a shim at all

The device is a UEFI machine: `boot_a` normally holds an Android boot image
whose "kernel" is a Linux `Image` built for the downstream 5.10 kernel, and the
DTB comes from `vendor_boot`, selected by `msm-id`/`board-id`.

Two problems for mainline bring-up:

1. We want to run **our own** kernel image, and we want to try hundreds of
   variants of it. Rebuilding an Android `vendor_boot` with matching
   `msm-id`/`board-id` every time is painful.
2. We want **our own DTB**. The vendor DTB contains downstream bindings and a
   memory map that a mainline kernel will partly reject.

So instead of fighting the bootloader, `boot_a` is filled with a tiny
freestanding ARM64 program (the *shim*) that the bootloader happily loads and
jumps to. The shim then:

1. prints its state to the framebuffer ABL already set up (so we get output with
   zero driver support),
2. finds a payload (`SWEETLNX` magic) appended behind its own image,
3. copies the real kernel `Image` to a configurable load address, zeroing the
   `.bss` gap that the file does not cover,
4. copies the DTB to a configurable address,
5. cleans up (MMU/caches off, `dsb`/`isb`), sets `x0 = dtb`, and branches to the
   kernel entry with `el=1`.

Verified on hardware: `sctlr_m=0`, `cntfrq=0x124F800`, framebuffer at
`0xB8000000` stride `0x10E0`, `_end=0xA8013000`, ABL loads `boot.img` at
`0xA8000000`.

## What is in `shim/`

| File | Purpose |
|------|---------|
| `start.S` | entry point, saves x0–x3, sets up a stack, calls `shim_main`, then `linux_boot` |
| `shim_sky.c` | finding/verifying the payload, load-address logic, parking stub for secondary CPUs |
| `loader.c` | copy + `.bss` zeroing (`zero64`) |
| `console.c/h` | minimal framebuffer text console (fixed 8×16 font, 2× scale) |
| `fdt.c/h`, `font.h` | tiny helpers |
| `shim.ld` | link script |
| `build-sky.sh` | builds the shim, rejects absolute (ABS64) relocations, embeds the payload, produces an Android v4 boot image |
| `tools/*.py` | payload/boot-image packers |

### Hard constraints the shim has to live with

* Runs at an arbitrary physical address (ABL puts it at `0xA8000000`), so **no
  absolute relocations** are allowed in the C objects. `build-sky.sh` fails the
  build if any `R_AARCH64_ABS64` relocation is present. This is why the console
  font is a 2-D `char[16][16]` table instead of an array of pointers.
* No libc, no MMU, no cache coherency setup beyond what ABL left.
* The kernel `Image` header's `image_size` is larger than the file (it includes
  `.bss`); the shim must zero that range or the kernel dies in mysterious ways.

## Payload format

`tools/mklinuxpayload.py Image.dtb payload.bin --load 0xc0000000` writes:

```
offset 0  : "SWEETLNX" magic + header (load address, image size, image_size,
            dtb offset, dtb size, flags)
offset 64 : raw Image bytes
...       : DTB bytes
```

`shim_sky.c` validates the magic and the sizes, then does the copies.

## Building and flashing

```bash
# 1. build the shim (produces out/shim.bin)
cd shim && ./build-sky.sh

# 2. pack a payload: kernel Image + DTB
python3 tools/mklinuxpayload.py /path/to/Image /path/to/board.dtb out/payload.bin \
        --load 0xc0000000

# 3. wrap it into a boot image (shim is the "kernel")
./build-sky.sh out/payload.bin        # -> out/sky-shim-boot-v4.img

# 4. flash boot_a only
fastboot flash boot_a out/sky-shim-boot-v4.img && fastboot reboot
```

`make-m1.sh` in the same directory does steps 2–4 in one shot.

## Screen layout

The shim paints a blue band with:

```
SHIM     sky M1 (SM4450 ravelin)
CPU      el=… cntfrq=…
STATE    sctlr_m=… abl_dtb=…
FB       base=… stride=…
_end     <shim end>
PAYLOAD  <address>
RESULT   SWEETLNX found
PAYLOAD  at=… size=…
IMAGE    size=… mem=…
LOAD     pa=… flags=…
DTB      pa=… size=…
BSS      zero=… at=…
LINUX    jumping
```

If you see `LINUX jumping` the shim did its job; anything after that is the
kernel's problem.

## Gotchas learned the hard way

* **`.bss` must be zeroed by hand** — the kernel's `image_size` includes it, the
  file does not. Symptom: random early crashes after `jumping`.
* **Zeroing must use 64-bit stores and be bounded** by `image_size`; the first
  version used a `u32` loop and was too slow to be useful for debugging.
* **No ABS64 relocations**: keep pointer tables out of the C code.
* The shim is *not* a bootloader: it does not touch clocks, regulators or the
  display. Whatever ABL left running keeps running — which is exactly what makes
  the framebuffer console work later (see M1's `clk_ignore_unused`).
