#!/bin/bash
# Build a .dts that #includes kernel bindings.
#
#   tools/dtsbuild.sh dts/sm4450-xiaomi-sky.dts [outdir]
#
# KDIR must point at a kernel tree that contains the ROCKNIX SM4450 patches
# (the board file includes sm4450.dtsi and the qcom dt-bindings headers).
set -e
DTS=$1
OUT=${2:-build}
KDIR=${KDIR:-../kernel/linux-7.3}
BASE=$(basename "$DTS" .dts)
mkdir -p "$OUT"
cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I "$KDIR/include" \
    -I "$KDIR/arch/arm64/boot/dts/qcom" \
    -I "$KDIR/include/dt-bindings" \
    -I "$KDIR/scripts/dtc/include-prefixes" \
    -I dts -I . \
    "$DTS" -o "$OUT/$BASE.pre.dts"
rm -f "$OUT/$BASE.dtb"
if ! dtc -I dts -O dtb -o "$OUT/$BASE.dtb" -Wno-unit_address_vs_reg \
        "$OUT/$BASE.pre.dts" 2>"$OUT/$BASE.dtc.log"; then
    head -25 "$OUT/$BASE.dtc.log"
    echo "DTC FAILED: $OUT/$BASE.dtb not produced" >&2
    exit 1
fi
echo "built: $OUT/$BASE.dtb ($(stat -c%s "$OUT/$BASE.dtb") bytes)"
