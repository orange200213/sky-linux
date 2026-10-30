# Local changes to the sm7150-mainline driver (`nt36xxx-ml/`)

Upstream: github.com/sm7150-mainline/linux @ fd78d17, files
`drivers/input/touchscreen/{nt36xxx.h,nt36xxx_core.c,nt36xxx_spi.c}`.

1. `nt36xxx_spi.c` — `of_match_table` gained our DT compatible:

   ```c
   { .compatible = "novatek,nvt-ts-spi", .data = &novatek_nt36xxx, },
   ```

   (upstream only matches `novatek,nt36xxx-spi`, so without this the module never binds;
   the `spi_device_id` table also got `{ "nvt-ts-spi", 0 }`).

2. `nt36xxx_spi.c` — read path rewritten to the vendor sequence. The vendor
   `CTP_SPI_READ()` sends the address byte and reads back `len + 1` bytes in **one
   full-duplex message**, taking the payload from `rx[2]`; upstream used
   `spi_write_then_read()` with no dummy bytes and got zeros.

   ```c
   #define NT36XXX_READ_DUMMY 2
   /* page select: { 0xff, (reg >> 15) & 0xff, (reg >> 7) & 0xff } */
   tx[0] = reg & 0x7f;
   xfer.tx_buf = tx; xfer.rx_buf = rx; xfer.len = val_size + NT36XXX_READ_DUMMY;
   spi_sync(spi, &msg);
   memcpy(val_buf, rx + NT36XXX_READ_DUMMY, val_size);
   ```

3. `nt36xxx_core.c` — chip-id comparison uses `buf[i]` (not `buf[i + 1]`, there is no
   leading dummy after the fix) and skips the empty sentinel table entry:

   ```c
   for (list = 0; list < sz; list++) {
           if (!trim_id_table[list].mapid)
                   continue;
   ```

   With the read fixed, our chip matches the enabled entry `{ 0x0C, 0xFF, 0xFF, 0x72, 0x66,
   0x03 }` → `NT36675_IC`, which is why the map in use (`nt36675_memory_maps`) is correct.

4. `nt36xxx_core.c` — IRQ made optional for bring-up (the TLMM cannot map gpio13):

   ```c
   ts->irq = ts->irq_gpio ? gpiod_to_irq(ts->irq_gpio) : -EINVAL;
   if (ts->irq <= 0) { dev_warn(dev, "no irq available, running without interrupts"); ts->irq = 0; }
   ...
   if (ts->irq > 0) { devm_request_threaded_irq(...); }
   ```

   For real use the DT must provide `irq-gpios = <&tlmm 13 GPIO_ACTIVE_LOW>;` and a
   working mapping.

5. The chip-id dump was switched from `dev_dbg` to `dev_info` so it is visible without
   `dyndbg`.

Build wiring: `nt36xxx-ml/Kconfig` (`CONFIG_TOUCHSCREEN_NT36XXX_ML`, tristate) sourced from
`drivers/input/touchscreen/Kconfig`, and `nt36xxx-ml/Makefile`:

```make
obj-$(CONFIG_TOUCHSCREEN_NT36XXX_ML) += nt36xxx-ml.o
nt36xxx-ml-y := nt36xxx_core.o nt36xxx_spi.o
```

(The distinct module name matters: `nt36xxx_core.o + nt36xxx_spi.o` would otherwise produce
`nt36xxx_spi.ko` and collide with the vendor driver.)
