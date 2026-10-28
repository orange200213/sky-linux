# Kernel build guide

## 1. Base tree

```bash
curl -LO https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.3.tar.xz
tar xf linux-7.3.tar.xz && cd linux-7.3
git init . && git add -A && git commit -m "Linux 7.3 baseline"
```

**Why 7.3 and not 7.2.11:** the ROCKNIX SM4450 series is written against
7.2.11. On 7.3 two patches need a small rebase (see below), everything else
applies cleanly. A 7.2.11 tree with the untouched series also builds and is the
lower-risk choice if you do not care about being on the newest release.

## 2. ROCKNIX SM4450 series

The community patches are not vendored here (they carry their own license
headers and are large). Fetch them from the ROCKNIX project:

```
projects/ROCKNIX/devices/SM4450/linux/patches/
```

Apply the whole set, or the subset listed below. The subset that is actually
needed for M1–M4:

| Patch | Why |
|-------|-----|
| `0002` rpmhpd LCX | power domains |
| `0004` regulator rpmh PM6450 | UFS/display rails |
| `0005` pd-mapper | ADSP/WPSS |
| `0006` interconnect sm4450 | bus voting (UFS, USB) |
| `0008` arm-smmu sm4450 | IOMMU compatible |
| `0011` dispcc fix, `0012` dispcc quiesce splash | display clocks |
| `0022` rpmh PBS | regulator PBS type |
| `0023` leds qcom-lpg PM6450 PWM | backlight path |
| `0024` geni serial rx restart | **not 7.3 compatible** — skip on 7.3 |
| `0040`/`0042` qmp combo (sm4450, no runtime suspend) | USB3/DP PHY (optional today) |
| `0043` phy-qcom-qmp-ufs sm4450 | **UFS** |
| `0049` ucsi_glink quirk | Type-C (optional) |
| `0051`/`0052` battmgr | charger info (optional) |
| `0057` sm4450 cpu capacity/power coeffs | scheduler |

### Two rebases needed on 7.3

1. **UBWC** (`0009`): the table format was refactored upstream into shared
   descriptors; the patch has to be re-expressed against
   `ubwc_2_0_hbb14`/`drivers/soc/qcom/ubwc_config.c`.
2. **RPMH PBS** (`0022`): `RPMH_REGULATOR_REG_PBS_ENABLE` moved; the patch has
   to add the definition in the new header location.

`0024` (geni serial RX) does not compile on 7.3 — it is not needed for our boot
path and should be dropped there.

## 3. Our patches

```
kernel/patches/0001-pinctrl-msm-tolerate-tz-protected-gpios.patch
kernel/patches/0002-ufs-qcom-add-sm4450-ufshc-compatible.patch
```

`0001` stops gpiolib from probing the direction of every pin at registration
(TZ-protected TLMM pages → synchronous external abort). `0002` gives the SM4450
UFS host the SM8550 quirk set so the controller survives idle
(`no_phy_retention`).

Debug-only instrumentation (framebuffer progress markers, panic painting, PHY
register tracing) lives in `kernel/patches-debug/` and should **not** be applied
to anything but a bring-up build.

## 4. Configuration

`kernel/configs/sky-m1.config` is the full `.config` used for the milestone
images. It is an arm64 `defconfig` with:

```
# storage
CONFIG_SCSI=y CONFIG_BLK_DEV_SD=y CONFIG_SCSI_UFSHCD=y CONFIG_SCSI_UFS_QCOM=y
CONFIG_PHY_QCOM_QMP=y CONFIG_PHY_QCOM_QMP_UFS=y CONFIG_EXT4_FS=y CONFIG_F2FS_FS=y

# usb gadget
CONFIG_USB=y CONFIG_USB_GADGET=y CONFIG_USB_DWC3=y CONFIG_USB_DWC3_QCOM=y
CONFIG_PHY_QCOM_USB_SNPS_FEMTO_V2=y
CONFIG_USB_CONFIGFS=y CONFIG_USB_CONFIGFS_ACM=y CONFIG_USB_CONFIGFS_NCM=y
CONFIG_USB_G_SERIAL=n            # must stay off, it would steal the UDC

# console
CONFIG_DRM=y CONFIG_DRM_SIMPLEDRM=y CONFIG_DRM_FBDEV_EMULATION=y
CONFIG_FB_CORE=y CONFIG_FRAMEBUFFER_CONSOLE=y

# crash logs
CONFIG_PSTORE=y CONFIG_PSTORE_RAM=y CONFIG_PSTORE_CONSOLE=y CONFIG_PSTORE_PMSG=y

# the SoC bits
CONFIG_SERIAL_QCOM_GENI=y CONFIG_PINCTRL_SM4450=y
CONFIG_INTERCONNECT_QCOM_SM4450=y CONFIG_QCOM_RPMH=y CONFIG_QCOM_RPMHPD=y
CONFIG_QCOM_SMEM=y CONFIG_QCOM_COMMAND_DB=y CONFIG_QCOM_SMD_RPM=y
CONFIG_REGULATOR_QCOM_RPMH=y
```

Gotchas:

* `PHY_QCOM_*` children inherit the tristate of `PHY_QCOM_QMP`. If the parent is
  `=m`, the child cannot be `=y` — and the initramfs has no module loader.
* Turning `DRM` off entirely also disables `simpledrm`, i.e. the console.
* `CONFIG_USB_G_SERIAL=y` + configfs gadgets = UDC already taken, no gadget.

Build:

```bash
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make olddefconfig
make -j"$(nproc)" Image dtbs
```

The initramfs is embedded via `CONFIG_INITRAMFS_SOURCE` (a directory); see
`initramfs/README.md`. Keep it out of the tree when you want a plain `Image`.

## 5. Device tree

```bash
cp dts/sm4450-xiaomi-sky.dts arch/arm64/boot/dts/qcom/
echo 'dtb-$(CONFIG_ARCH_QCOM) += sm4450-xiaomi-sky.dtb' \
     >> arch/arm64/boot/dts/qcom/Makefile
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- qcom/sm4450-xiaomi-sky.dtb
```

The file `#include`s mainline `sm4450.dtsi`, `pmk8350.dtsi`, `pm7250b.dtsi`, so
it must live inside the kernel tree. `tools/mksky_dts.py` documents how the
board parts were derived from sky's own runtime DT and the vendor DTB.

## 6. Packing and flashing

```bash
python3 tools/mklinuxpayload.py Image board.dtb payload.bin --load 0xc0000000
cd shim && ./build-sky.sh ../payload.bin      # -> out/sky-shim-boot-v4.img
fastboot flash boot_a out/sky-shim-boot-v4.img && fastboot reboot
```

Recovery: flash the stock `boot.img` back to `boot_a`.

## 7. Verified boot log (M1)

```
Booting Linux on physical CPU 0x0000000000 [0x412fd050]
Machine model: Xiaomi Redmi 12 5G (sky)
[drm] Initialized simpledrm 1.0.0 for b8000000.framebuffer on minor 0
Console: switching to colour frame buffer device 135x153
…
Freeing unused kernel memory: 23552K
Run /init as init process
```
