# Debugging without a UART

There are no accessible UART pads on this phone, so the project bootstrapped six
independent channels, in the order they became available. Each one is worth
keeping in the toolbox.

## 1. Framebuffer console (`fbcon`)

ABL leaves the splash framebuffer scanning out at `0xb8000000`
(1080×2460, stride 4320). A `simple-framebuffer` node under `/chosen` plus
`simpledrm` gives a real console with **no display driver at all**:

```
[drm] Initialized simpledrm 1.0.0 for b8000000.framebuffer on minor 0
Console: switching to colour frame buffer device 135x153
```

Prerequisites that were learned the hard way:

* `reg` of the node must be **at least** as large as the driver's computed size;
  give it the whole 43 MiB splash region (`0x2b00000`), not the exact
  `width*height*4`.
* the display dies a few hundred ms into boot unless
  `clk_ignore_unused pd_ignore_unused` are on the command line (otherwise Linux
  switches off the DSI clocks ABL left running).

## 2. Painted progress markers

`kernel/patches-debug/0001-…` adds `sky_dbg_mark(n)` to `init/main.c`: a 64×64
pixel block in a fixed colour at a fixed position, called from ~20 points in
`start_kernel()`/`init/main.c`. Because it writes straight into the
framebuffer, it works long before any console exists.

`0002-…` hooks `vpanic()`: the screen is filled with the **colour of the last
stage reached**, with a red bar on top.

Rules that keep this useful:

* always clamp `y` to the framebuffer height — writing one row past the end
  faults and looks exactly like a kernel hang (this bug cost several flash
  cycles);
* `ioremap()` must cover the full framebuffer (`resource_size()` of the DT node),
  not an arbitrary 2 MiB — writing past the mapping faults inside `panic()`,
  which looks like "panic never happened, device hung";
* keep a colour table big enough for the number of stages.

## 3. `pstore` / `ramoops`

`ramoops@97700000` (2 MiB) is described in the DTS. A panic stores the last
kernel log there; TWRP can read it afterwards:

```bash
adb pull /sys/fs/pstore/dmesg-ramoops-0.enc.z /tmp/rec.z
python3 - <<'PY'
import zlib
d = open('/tmp/rec.z','rb').read()
print(zlib.decompress(d, -15).decode('utf-8', 'replace'))
PY
```

Two practical notes:

* despite the `.enc` suffix, the record is plain **raw deflate** — no key needed;
* pstore only captures a panic, so during bring-up a "log capture" image ends
  `init` with `echo c > /proc/sysrq-trigger`. That turns any boot into a
  readable log.

The `pmsg` record (`pmsg-ramoops-0`) is shared with Android/TWRP and was not
usable as a log sink.

## 4. USB CDC-ACM serial console

`/dev/ttyGS0` on the device ↔ `/dev/ttyACM0` on the host, with a root shell
attached (see M2). This is the channel that ended the "photograph the screen"
era.

```bash
sudo picocom -b 115200 /dev/ttyACM0
```

## 5. USB NCM + SSH

`192.168.7.1`, DHCP server on the device, OpenSSH (see M3).

```bash
ssh root@192.168.7.1
```

## 6. Persistent log on UFS

`/mnt/log/dmesg-last.txt`, rewritten at every boot (see M4). Reading the log of
the *previous* boot is often what you actually want.

## Practical lessons

* A "hang" is usually one of: (a) a fault in your own debug code, (b) an
  unwritten framebuffer row, (c) a clock that Linux switched off, (d) an
  XPU-protected register access. Check those before suspecting the driver.
* Synchronous external aborts (ESR `0x960000xx`, FSC `0x10`) are **not**
  fixable with `copy_from_kernel_nofault()`; avoid the access instead.
* `PHY_QCOM_*` tristates inherit from `PHY_QCOM_QMP`; with no module loader,
  a `=m` child silently means "not available".
* When a build system "succeeds" but behaviour does not change, check that the
  artefact was actually regenerated: our DTS build printed `built: …` while
  `dtc` had aborted, so several images shipped a stale DTB. `tools/dtsbuild.sh`
  now deletes the output and exits non-zero on failure.
