#!/usr/bin/env python3
"""Generate dts/sm4450-xiaomi-sky.dts from the ROCKNIX Anbernic RG55G1 board file.

The RG55G1 is the same SoC (SM4450) with the same reference memory map, so its
board file is used as the SoC-node donor. Board specific parts are replaced
with the values read from sky's runtime DT (see docs/sky-hw-params.json).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import dts_blocks as B  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "rocknix", "dts", "sm4450-anbernic-rg55g1.dts")
OUT = os.path.join(ROOT, "dts", "sm4450-xiaomi-sky.dts")

HEADER = """// SPDX-License-Identifier: BSD-3-Clause
/*
 * Xiaomi Redmi 12 5G (sky) - Qualcomm SM4450 (ravelin / Snapdragon 4 Gen 2).
 *
 * SKELETON. Generated from ROCKNIX's sm4450-anbernic-rg55g1.dts (same SoC,
 * same reference memory map) with the board specific parts replaced by the
 * values read out of sky's own runtime DT (docs/sky-hw-params.json).
 *
 * It is meant as the starting point for the first boot, not as a finished
 * board file. Known TODOs:
 *   - memory: sky's /memory node still has to be read back on the device
 *     (/proc/device-tree/memory/reg); the region below is a placeholder that
 *     matches the 4 GB QRD layout this DT came from.
 *   - panel: no driver for xiaomi,m19-36-02-0a yet; timings/DSC come from the
 *     vendor DT. Backlight is a KTD3136 on i2c (no mainline driver).
 *   - touch: Novatek NVT on SPI (downstream driver only), node added at the
 *     bottom of the file.
 *   - regulators: inherited from the RG55G1 file ("live parrot DT"); they must
 *     be diffed against sky's own rails before enabling UFS/display/GPU.
 *   - anbernic-only peripherals (fan, hall sensor, joypad, RGB LEDs, WCN3950
 *     WiFi/BT) are still present and have to be deleted or disabled.
 *   - UFS inline crypto (ICE) is not described.
 */

"""

PANEL = """\t\t\tpanel: panel@0 {
\t\t\t\tcompatible = "xiaomi,m19-36-02-0a-fhd-dsc";
\t\t\t\treg = <0>;

\t\t\t\tvddio-supply = <&display_panel_vddio>;
\t\t\t\textvdd-supply = <&display_panel_extvdd>;
\t\t\t\tavdd-supply = <&display_panel_avdd>;
\t\t\t\tibb-supply = <&display_panel_ibb>;

\t\t\t\treset-gpios = <&tlmm 92 GPIO_ACTIVE_LOW>;
\t\t\t\tpinctrl-0 = <&panel_reset_default>;
\t\t\t\tpinctrl-names = "default";

\t\t\t\trotation = <90>;

\t\t\t\tqcom,mdss-dsi-video-mode;
\t\t\t\tqcom,mdss-dsi-panel-width = <1080>;
\t\t\t\tqcom,mdss-dsi-panel-height = <2460>;
\t\t\t\tqcom,mdss-dsi-h-front-porch = <59>;
\t\t\t\tqcom,mdss-dsi-h-back-porch = <60>;
\t\t\t\tqcom,mdss-dsi-h-pulse-width = <12>;
\t\t\t\tqcom,mdss-dsi-h-sync-skew = <0>;
\t\t\t\tqcom,mdss-dsi-v-front-porch = <54>;
\t\t\t\tqcom,mdss-dsi-v-back-porch = <10>;
\t\t\t\tqcom,mdss-dsi-v-pulse-width = <10>;
\t\t\t\tqcom,mdss-dsi-panel-framerate = <90>;
\t\t\t\tqcom,mdss-dsi-t-clk-post = <0x0f>;
\t\t\t\tqcom,mdss-dsi-t-clk-pre = <0x1d>;

\t\t\t\t/* DSC: 8 bpp, 8 bpc, 2 slices per packet, 540x20 slices */
\t\t\t\tqcom,mdss-dsc-version = <0x11>;
\t\t\t\tqcom,mdss-dsc-scr-version = <0x0>;
\t\t\t\tqcom,mdss-dsc-bit-per-component = <8>;
\t\t\t\tqcom,mdss-dsc-bit-per-pixel = <8>;
\t\t\t\tqcom,mdss-dsc-block-prediction-enable;
\t\t\t\tqcom,mdss-dsc-slice-per-pkt = <2>;
\t\t\t\tqcom,mdss-dsc-slice-width = <540>;
\t\t\t\tqcom,mdss-dsc-slice-height = <20>;

\t\t\t\tport {
\t\t\t\t\tpanel_in: endpoint {
\t\t\t\t\t\tremote-endpoint = <&mdss_dsi0_out>;
\t\t\t\t\t};
\t\t\t\t};
\t\t\t};
"""

BOARD_TAIL = """
/*
 * ---------------------------------------------------------------------------
 * sky board additions
 * ---------------------------------------------------------------------------
 */

/ {
\t/* Display rails: vendor DT "display_gpio_regulator@0..2" +
\t * "display_panel_ibb_stub". GPIOs: vddio = PM6450 gpio11,
\t * extvdd = PM6450 gpio12, avdd = TLMM gpio127, ibb = ABL/labibb.
\t */
\tdisplay_panel_vddio: regulator-panel-vddio {
\t\tcompatible = "regulator-fixed";
\t\tregulator-name = "display_panel_vddio";
\t\tregulator-min-microvolt = <1800000>;
\t\tregulator-max-microvolt = <1800000>;
\t\tgpio = <&pm6450_gpios 11 GPIO_ACTIVE_HIGH>;
\t\tenable-active-high;
\t\tregulator-boot-on;
\t};

\tdisplay_panel_extvdd: regulator-panel-extvdd {
\t\tcompatible = "regulator-fixed";
\t\tregulator-name = "display_panel_extvdd";
\t\tregulator-min-microvolt = <1200000>;
\t\tregulator-max-microvolt = <1200000>;
\t\tgpio = <&pm6450_gpios 12 GPIO_ACTIVE_HIGH>;
\t\tenable-active-high;
\t\tregulator-boot-on;
\t};

\tdisplay_panel_avdd: regulator-panel-avdd {
\t\tcompatible = "regulator-fixed";
\t\tregulator-name = "display_panel_avdd";
\t\tregulator-min-microvolt = <5500000>;
\t\tregulator-max-microvolt = <5500000>;
\t\tgpio = <&tlmm 127 GPIO_ACTIVE_HIGH>;
\t\tenable-active-high;
\t\tregulator-boot-on;
\t};

\t/* no control GPIO in the vendor DT: the rail is left as the bootloader
\t * programmed it until the panel driver knows better */
\tdisplay_panel_ibb: regulator-panel-ibb {
\t\tcompatible = "regulator-fixed";
\t\tregulator-name = "display_panel_ibb";
\t\tregulator-min-microvolt = <4600000>;
\t\tregulator-max-microvolt = <6000000>;
\t\tregulator-boot-on;
\t};

\t/* pstore across resets (vendor: ramoops@97700000) */
\treserved-memory {
\t\tramoops_region: ramoops@97700000 {
\t\t\tcompatible = "ramoops";
\t\t\treg = <0x0 0x97700000 0x0 0x200000>;
\t\t\trecord-size = <0x20000>;
\t\t\tconsole-size = <0x100000>;
\t\t\tpmsg-size = <0x40000>;
\t\t\tno-map;
\t\t};
\t};
};

/*
 * Touch: Novatek NVT on SPI (vendor: spi@a84000, SE1 of QUPv3 wrapper 1).
 * Reset GPIO12, IRQ GPIO13, 1080x2460. Kept out of the geniqup wrapper for
 * now so that the skeleton stays readable; move it under qupv3_id_0 later.
 */
&soc {
\tspi_touch: spi@a84000 {
\t\tcompatible = "qcom,geni-spi";
\t\treg = <0x0 0x00a84000 0x0 0x4000>;
\t\tclocks = <&gcc GCC_QUPV3_WRAP1_S1_CLK>;
\t\tclock-names = "se";
\t\tinterrupts = <GIC_SPI 602 IRQ_TYPE_LEVEL_HIGH>;
\t\tpinctrl-0 = <&qup_spi1_default>;
\t\tpinctrl-names = "default";
\t\t#address-cells = <1>;
\t\t#size-cells = <0>;
\t\tstatus = "disabled"; /* enable once a driver exists */

\t\ttouchscreen@0 {
\t\t\tcompatible = "novatek,nvt-ts-spi";
\t\t\treg = <0>;
\t\t\tspi-max-frequency = <9600000>;
\t\t\tinterrupt-parent = <&tlmm>;
\t\t\tinterrupts = <13 IRQ_TYPE_EDGE_FALLING>;
\t\t\treset-gpios = <&tlmm 12 GPIO_ACTIVE_LOW>;
\t\t\ttouchscreen-size-x = <1080>;
\t\t\ttouchscreen-size-y = <2460>;
\t\t};
\t};
};

&tlmm {
\tpanel_reset_default_sky: panel-reset-default-state {
\t\tpins = "gpio92";
\t\tfunction = "gpio";
\t\tdrive-strength = <8>;
\t\tbias-disable;
\t\toutput-high;
\t};

\tqup_spi1_default: qup-spi1-default-state {
\t\tmosi-pins {
\t\t\tpins = "gpio8";
\t\t\tfunction = "qup1_se1";
\t\t\tdrive-strength = <6>;
\t\t\tbias-disable;
\t\t};
\t\tmiso-pins {
\t\t\tpins = "gpio9";
\t\t\tfunction = "qup1_se1";
\t\t\tdrive-strength = <6>;
\t\t\tbias-disable;
\t\t};
\t\tclk-pins {
\t\t\tpins = "gpio10";
\t\t\tfunction = "qup1_se1";
\t\t\tdrive-strength = <6>;
\t\t\tbias-disable;
\t\t};
\t\tcs-pins {
\t\t\tpins = "gpio11";
\t\t\tfunction = "qup1_se1";
\t\t\tdrive-strength = <2>;
\t\t\tbias-disable;
\t\t\toutput-high;
\t\t};
\t};
};
"""


MEM_TAIL = """
/* mainline's sm4450.dtsi expects the bootloader to fill /memory in; the shim
 * passes the DTB as-is, so provide the low bank here (M1 placeholder: first
 * region of sky's vendor /memory, see docs/C-sky-hw-params.md section 2). */
/ {
	/delete-node/ memory@a0000000;

	memory@80000000 {
		device_type = "memory";
		reg = <0x0 0x80000000 0x0 0x68e00000>;
	};
};
"""


def main():
    text = open(SRC).read()

    # 1. model / compatible
    text = re.sub(r'\tmodel = "[^"]*";', '\tmodel = "Xiaomi Redmi 12 5G (sky)";', text, count=1)
    text = re.sub(r'\tcompatible = "qcom,sm4450[^"]*"[^;]*;',
                  '\tcompatible = "xiaomi,sky", "xiaomi,river", "qcom,sm4450", "qcom,ravelin";', text, count=1)

    # 2. panel node
    blocks = {lbl: (s, e) for lbl, name, s, e, ind in B.blocks(text)}
    if "panel" not in blocks:
        raise SystemExit("panel node not found")
    s, e = blocks["panel"]
    text = text[:s] + PANEL.rstrip("\n") + text[e:]

    # 3. drop the anbernic touchscreen (sky uses SPI / Novatek)
    blocks = {lbl: (s, e) for lbl, name, s, e, ind in B.blocks(text)}
    for lbl, name, s, e, ind in B.blocks(text):
        if name.startswith("touchscreen@"):
            text = text[:s] + "/* sky: touch is on SPI, see spi@a84000 below */\n\t\t\t" + text[e:]
            break

    # 4a. drop the RG55G1 ramoops (sky has its own address)
    for lbl, name, s0, e0, ind in B.blocks(text):
        if name.startswith("ramoops@"):
            text = text[:s0] + text[e0:]
            break

    # 4. reserved-memory sizes that differ on sky
    text = text.replace("reg = <0x0 0x87300000 0x0 0x2100000>;", "reg = <0x0 0x87300000 0x0 0x2900000>;")
    text = text.replace("reg = <0x0 0x8a000000 0x0 0xda00000>;", "reg = <0x0 0x8a000000 0x0 0xd700000>;")

    # 4b. drop anbernic-only peripherals (handheld: fan, hall, joypad, RGB,
    #     PWM backlight, WCD937x/AW88166 sound card)
    drop_labels = {"fan", "fan_pwm", "fan_pwr", "mcu_vdd", "hall_vdd", "rgb_vdd",
                   "singleadc_joypad", "speaker_amp_l", "speaker_amp_r", "sound",
                   "backlight", "leds", "wcn3950-pmu"}
    ranges = []
    for lbl, name, s0, e0, ind in B.blocks(text):
        if lbl in drop_labels or name in drop_labels:
            ranges.append((s0, e0, lbl or name))
    for s0, e0, what in sorted(ranges, reverse=True):
        text = text[:s0] + text[e0:]
        print("dropped:", what)

    # 4c. WCN3950 -> WCN6750 (sky's WiFi/BT combo), both disabled until the
    #     WPSS/ath11k work is done
    for lbl, name, s0, e0, ind in B.blocks(text):
        if lbl == "wifi":
            blk = text[s0:e0]
            blk = blk.replace('compatible = "qcom,wcn3990-wifi";',
                              'compatible = "qcom,wcn6750-wifi";')
            blk = blk.replace('status = "okay";',
                              'status = "disabled"; /* enable with WPSS/ath11k */')
            text = text[:s0] + blk + text[e0:]
            break
    for lbl, name, s0, e0, ind in B.blocks(text):
        if name == "bluetooth":
            blk = text[s0:e0]
            blk = blk.replace('compatible = "qcom,wcn3950-bt";',
                              'compatible = "qcom,wcn6750-bt";\n\t\t\t\tstatus = "disabled"; /* enable with WPSS/ath11k */')
            # sky's WCN6750 rails (vendor bt_wcn6750), see docs/C-sky-hw-params.md
            blk = blk.replace('vddio-supply = <&vreg_pmu_io>;', 'qcom,bt-vdd-io-supply = <&vreg_l21b_1p8>;')
            blk = blk.replace('vddxo-supply = <&vreg_pmu_xo>;',
                              'qcom,bt-vdd-aon-supply = <&vreg_s7b_1p12>;\n\t\t\t\tqcom,bt-vdd-dig-supply = <&vreg_s7b_1p12>;')
            blk = blk.replace('vddrf-supply = <&vreg_pmu_rf>;', 'qcom,bt-vdd-rfa1-supply = <&vreg_s9b_1p872>;')
            blk = blk.replace('vddch0-supply = <&vreg_pmu_ch0>;', 'qcom,bt-vdd-rfa2-supply = <&vreg_s8b_1p352>;')
            text = text[:s0] + blk + text[e0:]
            break

    # 4d. simple-framebuffer for the ABL splash: sky's panel is 1080x2460,
    #     the RG55G1 one 1080x1920 (anbernic's node), and give Linux a cmdline
    for lbl, name, s0, e0, ind in B.blocks(text):
        if name == "chosen":
            blk = text[s0:e0]
            blk = blk.replace("reg = <0x0 0xb8000000 0x0 0x7e9000>;",
                              "reg = <0x0 0xb8000000 0x0 0x2b00000>; /* whole ABL splash region */")
            blk = blk.replace("height = <1920>;", "height = <2460>;")
            blk = blk.replace("format = \"x8r8g8b8\";",
                              "format = \"x8r8g8b8\";\n\t\t\tstatus = \"okay\";")
            blk = blk.replace("\t\tranges;",
                              "\t\tranges;\n\t\tbootargs = \"console=tty0 fbcon=nodefer loglevel=7 ignore_loglevel nokaslr panic=15 clk_ignore_unused pd_ignore_unused console=ttyGS0\";")
            text = text[:s0] + blk + text[e0:]
            break

    # 4e. USB: force peripheral mode. With dr_mode=otg + role switch the dwc3
    # gadget start fails ("failed to enable ep0out"), so bypass the role
    # switch entirely (pmic-glink stays enabled, it just has no client).
    text = text.replace('dr_mode = "otg";', 'dr_mode = "peripheral";')
    text = text.replace("\t\tusb-role-switch;\n", "")
    text = text.replace("\t\trole-switch-default-mode = \"peripheral\";\n", "")
    # no USB3/DP QMP phy on this build: tell the glue to use the UTMI clock as
    # the pipe clock, otherwise dwc3 cannot start its gadget ("ep0out" timeout)
    text = text.replace('\t\tdr_mode = "peripheral";\n',
                        '\t\tdr_mode = "peripheral";\n\t\tqcom,select-utmi-as-pipe-clk;\n')

    # 4f. bring-up noise: the panel rails cannot resolve their PMIC GPIOs yet
    # (no panel driver), and the GPU SMMU probe times out for 10 s. Keep both
    # out of the way until the display/GPU work starts.
    for rail in ("display_panel_vddio", "display_panel_extvdd"):
        text = text.replace(
            "\t\tgpio = <&pm6450_gpios 11 GPIO_ACTIVE_HIGH>;\n", "")
        text = text.replace(
            "\t\tgpio = <&pm6450_gpios 12 GPIO_ACTIVE_HIGH>;\n", "")
    m = re.search(r"\tgpu: gpu@3d00000 \{.*?\n\t\};", text, flags=re.S)
    if m:
        blk = re.sub(r"\n\t\tstatus = \"[^\"]*\";", "", m.group(0))
        blk = blk.replace("\tgpu: gpu@3d00000 {",
                          "\tgpu: gpu@3d00000 {\n\t\tstatus = \"disabled\";", 1)
        text = text[:m.start()] + blk + text[m.end():]

    # 4g. USB bring-up: drop the USB3/DP QMP phy, the pmic-glink role switch
    # and the apps-smmu stream mapping. All three must probe before dwc3 will
    # register its UDC; without them a plain HS peripheral gadget is enough.
    text = text.replace(
        "\t\tphys = <&usb_1_hsphy>,\n"
        "\t\t       <&usb_1_qmpphy QMP_USB43DP_USB3_PHY>;\n"
        "\t\tphy-names = \"usb2-phy\", \"usb3-phy\";\n",
        "\t\tphys = <&usb_1_hsphy>;\n"
        "\t\tphy-names = \"usb2-phy\";\n")
    text = text.replace("\t\tiommus = <&apps_smmu 0x540 0x0>;\n", "")

    # 5. header + board tail
    text = HEADER + text + BOARD_TAIL + MEM_TAIL
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write(text)
    print("wrote", OUT, len(text.splitlines()), "lines")


if __name__ == "__main__":
    main()
