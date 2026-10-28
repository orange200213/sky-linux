# tools

| Tool | Purpose |
|------|---------|
| `mklinuxpayload.py` | appends a DTB behind a kernel `Image` and writes the `SWEETLNX` header the shim expects (`--load` sets the kernel load address) |
| `mkbootimg_v3.py` | builds an Android v4 boot image from an arbitrary "kernel" blob (the shim + payload) |
| `dtsbuild.sh` | preprocesses a `.dts` with the kernel's bindings (`KDIR=…`) and runs `dtc`; fails loudly and deletes the output if `dtc` errors |
| `mksky_dts.py` | generates `dts/sm4450-xiaomi-sky.dts` from the ROCKNIX Anbernic board file, patching in sky-specific nodes (memory, framebuffer, USB mode, panel rails, GPU/UFS status) |
| `dts_parse.py` | small DTS parser used by the generator and by the hardware audit |

Typical pipeline:

```bash
python3 tools/mksky_dts.py                     # regenerate the board DTS
KDIR=/path/to/linux-7.3 tools/dtsbuild.sh dts/sm4450-xiaomi-sky.dts build
python3 tools/mklinuxpayload.py Image build/sm4450-xiaomi-sky.dtb payload.bin \
        --load 0xc0000000
cd shim && ./build-sky.sh ../payload.bin       # -> out/sky-shim-boot-v4.img
```
