# handoff13 — touch runtime recovery (2026-11-01)

`FINDINGS.md` is the write-up, `nt36xxx-runtime-recovery.patch` the driver change and
`SHA256SUMS` covers the full local archive.

The complete archive (kernel `Image`, DTB, driver sources and the 120 MB boot image)
stays out of git; it lives on the build machine at

    sky-mainline/build/archives/handoff13-runtime-recovery-20261101/

Reference image:

    images/sky-shim-boot-v4-recover-hwreset.img
    sha256 8ed569ca617688e8037a48ff0b4e54531bda3c55e0e7b731fd33b1b571c56bac
    size   120086528

The boot-time-validated predecessor (no runtime recovery) is
`shim/out/sky-shim-boot-v4-touch-ok.img`, sha256 `a16ce9e9…`.
