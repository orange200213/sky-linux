# Where the driver sources are

The driver in this handoff package is vendored at

    kernel/drivers/touchscreen/nt36xxx/{nt36xxx_core.c,nt36xxx.h,nt36xxx_spi.c}

(sha256 of the 2026-11-04 build: core `62bf354e0f0b7717…`, header `ffdce5555f3cb915…`,
spi `0061c03561b526c7…`). The `Kconfig`/`Makefile` hooks are already in place in the main
repository — note that this repository keeps only the subdirectory `Kconfig` (sourced from
`drivers/input/touchscreen/Kconfig`), so the duplicate-symbol problem described in the
package README does not apply here.
