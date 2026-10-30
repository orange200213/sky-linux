// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi Redmi 12 5G (sky) panel: "m19" 6.79" IPS LCD, 1080x2460, 396 ppi,
 * 24 bpp, 90 Hz, DSI video mode (DSC capable). Timings, physical size and the
 * 163-command power-on sequence were extracted from the vendor device tree
 * (see docs/panel-m19-init.json in the sky-linux repo).
 *
 * The backlight is a Kinetik KTD3136 on i2c (0x36) which has no mainline
 * driver yet; ABL leaves it enabled, so the panel is lit but at a fixed
 * brightness until that driver exists.
 */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define DSC_BPG_OFFSET(x)	((u8)((x) & DSC_RANGE_BPG_OFFSET_MASK))

struct m19_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[3];
	struct backlight_device *bl;
	struct drm_dsc_config dsc;
	bool prepared;
};

static unsigned int clock_khz = 276180;
module_param(clock_khz, uint, 0644);
MODULE_PARM_DESC(clock_khz, "pixel clock in kHz (DSI rate sweep)");

static const struct drm_display_mode m19_mode = {
	.clock = 276180,   /* 90 Hz with the vendor porches */
	.hdisplay = 1080,
	.hsync_start = 1080 + 59,
	.hsync_end = 1080 + 59 + 12,
	.htotal = 1080 + 59 + 12 + 60,
	.vdisplay = 2460,
	.vsync_start = 2460 + 54,
	.vsync_end = 2460 + 54 + 10,
	.vtotal = 2460 + 54 + 10 + 10,
	.width_mm = 69,   /* 69.33 mm */
	.height_mm = 158, /* 157.92 mm */
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct drm_dsc_config m19_dsc = {
	.dsc_version_major = 1,
	.dsc_version_minor = 1,   /* vendor table is the DSC 1.1 pre-SCR set */
	.slice_height = 20,
	.slice_width = 540,
	.slice_count = 2,
	.bits_per_component = 8,
	.bits_per_pixel = 8 << 4,
	.block_pred_enable = true,   /* vendor: qcom,mdss-dsc-block-prediction-enable */
	.simple_422 = false,
	.native_420 = false,
	.native_422 = false,
	.convert_rgb = 0,
	.vbr_enable = false,
	.pic_width = 1080,
	.pic_height = 2460,
	.rc_buf_thresh = {14, 28, 42, 56, 70, 84, 98, 105, 112, 119, 121, 123, 125, 126},
	.rc_model_size = DSC_RC_MODEL_SIZE_CONST,
	.rc_edge_factor = DSC_RC_EDGE_FACTOR_CONST,
	.rc_tgt_offset_high = DSC_RC_TGT_OFFSET_HI_CONST,
	.rc_tgt_offset_low = DSC_RC_TGT_OFFSET_LO_CONST,
	.mux_word_size = DSC_MUX_WORD_SIZE_8_10_BPC,
	.line_buf_depth = 9,
	.first_line_bpg_offset = 12,
	.initial_xmit_delay = 512,
	.initial_offset = 6144,
	.rc_quant_incr_limit0 = 11,
	.rc_quant_incr_limit1 = 11,
	.rc_range_params = {
		{ 0,  4, DSC_BPG_OFFSET(2)},
		{ 0,  4, DSC_BPG_OFFSET(0)},
		{ 1,  5, DSC_BPG_OFFSET(0)},
		{ 1,  6, DSC_BPG_OFFSET(-2)},
		{ 3,  7, DSC_BPG_OFFSET(-4)},
		{ 3,  7, DSC_BPG_OFFSET(-6)},
		{ 3,  7, DSC_BPG_OFFSET(-8)},
		{ 3,  8, DSC_BPG_OFFSET(-8)},
		{ 3,  9, DSC_BPG_OFFSET(-8)},
		{ 3, 10, DSC_BPG_OFFSET(-10)},
		{ 5, 11, DSC_BPG_OFFSET(-10)},
		{ 5, 12, DSC_BPG_OFFSET(-12)},
		{ 5, 13, DSC_BPG_OFFSET(-12)},
		{ 7, 13, DSC_BPG_OFFSET(-12)},
		{ 13, 15, DSC_BPG_OFFSET(-12)},
	},
};

static void m19_reset(struct m19_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static void m19_dcs_long_write(struct mipi_dsi_multi_context *ctx,
			       const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_DCS_LONG_WRITE,
		.tx_buf = data,
		.tx_len = len,
	};
	ssize_t ret;

	if (ctx->accum_err)
		return;

	if (dsi->mode_flags & MIPI_DSI_MODE_LPM)
		msg.flags |= MIPI_DSI_MSG_USE_LPM;

	ret = dsi->host->ops->transfer(dsi->host, &msg);
	if (ret < 0) {
		dev_err(&dsi->dev, "DCS long write %02x failed: %zd\n",
			*(const u8 *)data, ret);
		ctx->accum_err = ret;
	}
}

/* The vendor sequence uses long packets even for two-byte writes. */
#define m19_dcs_long_write_seq(ctx, seq...) do { \
	static const u8 data[] = { seq }; \
	m19_dcs_long_write(ctx, data, ARRAY_SIZE(data)); \
} while (0)

static int m19_on(struct m19_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	struct drm_dsc_picture_parameter_set pps;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	m19_reset(ctx);

	/* panel power-on sequence, straight from the vendor DT */
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0x20);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x69, 0x90);
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0x23);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x00, 0x60);
	m19_dcs_long_write_seq(&dsi_ctx, 0x01, 0x84);
	m19_dcs_long_write_seq(&dsi_ctx, 0x05, 0x2d);
	m19_dcs_long_write_seq(&dsi_ctx, 0x06, 0x00);
	m19_dcs_long_write_seq(&dsi_ctx, 0x07, 0x20);
	m19_dcs_long_write_seq(&dsi_ctx, 0x08, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x09, 0xbe);
	m19_dcs_long_write_seq(&dsi_ctx, 0x11, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x12, 0x9f);
	m19_dcs_long_write_seq(&dsi_ctx, 0x15, 0x65);
	m19_dcs_long_write_seq(&dsi_ctx, 0x16, 0x0b);
	m19_dcs_long_write_seq(&dsi_ctx, 0x0a, 0xff);
	m19_dcs_long_write_seq(&dsi_ctx, 0x0b, 0xff);
	m19_dcs_long_write_seq(&dsi_ctx, 0x0c, 0xff);
	m19_dcs_long_write_seq(&dsi_ctx, 0x0d, 0x3f);
	m19_dcs_long_write_seq(&dsi_ctx, 0x19, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x1a, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x1b, 0x04);
	m19_dcs_long_write_seq(&dsi_ctx, 0x1c, 0x05);
	m19_dcs_long_write_seq(&dsi_ctx, 0x1d, 0x05);
	m19_dcs_long_write_seq(&dsi_ctx, 0x1e, 0x05);
	m19_dcs_long_write_seq(&dsi_ctx, 0x1f, 0x06);
	m19_dcs_long_write_seq(&dsi_ctx, 0x20, 0x07);
	m19_dcs_long_write_seq(&dsi_ctx, 0x21, 0x08);
	m19_dcs_long_write_seq(&dsi_ctx, 0x22, 0x09);
	m19_dcs_long_write_seq(&dsi_ctx, 0x23, 0x0a);
	m19_dcs_long_write_seq(&dsi_ctx, 0x24, 0x0b);
	m19_dcs_long_write_seq(&dsi_ctx, 0x25, 0x0b);
	m19_dcs_long_write_seq(&dsi_ctx, 0x26, 0x0b);
	m19_dcs_long_write_seq(&dsi_ctx, 0x27, 0x0b);
	m19_dcs_long_write_seq(&dsi_ctx, 0x28, 0x0b);
	m19_dcs_long_write_seq(&dsi_ctx, 0x29, 0x04);
	m19_dcs_long_write_seq(&dsi_ctx, 0x2a, 0x40);
	m19_dcs_long_write_seq(&dsi_ctx, 0x2b, 0x09);
	m19_dcs_long_write_seq(&dsi_ctx, 0x30, 0xff);
	m19_dcs_long_write_seq(&dsi_ctx, 0x31, 0xfe);
	m19_dcs_long_write_seq(&dsi_ctx, 0x32, 0xfd);
	m19_dcs_long_write_seq(&dsi_ctx, 0x33, 0xfb);
	m19_dcs_long_write_seq(&dsi_ctx, 0x34, 0xf8);
	m19_dcs_long_write_seq(&dsi_ctx, 0x35, 0xf5);
	m19_dcs_long_write_seq(&dsi_ctx, 0x36, 0xf3);
	m19_dcs_long_write_seq(&dsi_ctx, 0x37, 0xf2);
	m19_dcs_long_write_seq(&dsi_ctx, 0x38, 0xf2);
	m19_dcs_long_write_seq(&dsi_ctx, 0x39, 0xf2);
	m19_dcs_long_write_seq(&dsi_ctx, 0x3a, 0xef);
	m19_dcs_long_write_seq(&dsi_ctx, 0x3b, 0xec);
	m19_dcs_long_write_seq(&dsi_ctx, 0x3d, 0xe9);
	m19_dcs_long_write_seq(&dsi_ctx, 0x3f, 0xe5);
	m19_dcs_long_write_seq(&dsi_ctx, 0x40, 0xe5);
	m19_dcs_long_write_seq(&dsi_ctx, 0x41, 0xe5);
	m19_dcs_long_write_seq(&dsi_ctx, 0x45, 0xff);
	m19_dcs_long_write_seq(&dsi_ctx, 0x46, 0xfe);
	m19_dcs_long_write_seq(&dsi_ctx, 0x47, 0xe6);
	m19_dcs_long_write_seq(&dsi_ctx, 0x48, 0xce);
	m19_dcs_long_write_seq(&dsi_ctx, 0x49, 0xb6);
	m19_dcs_long_write_seq(&dsi_ctx, 0x4a, 0xb2);
	m19_dcs_long_write_seq(&dsi_ctx, 0x4b, 0xa7);
	m19_dcs_long_write_seq(&dsi_ctx, 0x4c, 0x9d);
	m19_dcs_long_write_seq(&dsi_ctx, 0x4d, 0x92);
	m19_dcs_long_write_seq(&dsi_ctx, 0x4e, 0x88);
	m19_dcs_long_write_seq(&dsi_ctx, 0x4f, 0x7d);
	m19_dcs_long_write_seq(&dsi_ctx, 0x50, 0x73);
	m19_dcs_long_write_seq(&dsi_ctx, 0x51, 0x68);
	m19_dcs_long_write_seq(&dsi_ctx, 0x52, 0x66);
	m19_dcs_long_write_seq(&dsi_ctx, 0x53, 0x66);
	m19_dcs_long_write_seq(&dsi_ctx, 0x54, 0x66);
	m19_dcs_long_write_seq(&dsi_ctx, 0x58, 0xff);
	m19_dcs_long_write_seq(&dsi_ctx, 0x59, 0xfe);
	m19_dcs_long_write_seq(&dsi_ctx, 0x5a, 0xf3);
	m19_dcs_long_write_seq(&dsi_ctx, 0x5b, 0xe7);
	m19_dcs_long_write_seq(&dsi_ctx, 0x5c, 0xdc);
	m19_dcs_long_write_seq(&dsi_ctx, 0x5d, 0xd8);
	m19_dcs_long_write_seq(&dsi_ctx, 0x5e, 0xd3);
	m19_dcs_long_write_seq(&dsi_ctx, 0x5f, 0xcd);
	m19_dcs_long_write_seq(&dsi_ctx, 0x60, 0xc8);
	m19_dcs_long_write_seq(&dsi_ctx, 0x61, 0xc3);
	m19_dcs_long_write_seq(&dsi_ctx, 0x62, 0xbe);
	m19_dcs_long_write_seq(&dsi_ctx, 0x63, 0xb8);
	m19_dcs_long_write_seq(&dsi_ctx, 0x64, 0xb3);
	m19_dcs_long_write_seq(&dsi_ctx, 0x65, 0xb2);
	m19_dcs_long_write_seq(&dsi_ctx, 0x66, 0xb2);
	m19_dcs_long_write_seq(&dsi_ctx, 0x67, 0xb2);
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0x24);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x4d, 0x03);
	m19_dcs_long_write_seq(&dsi_ctx, 0x4e, 0x39);
	m19_dcs_long_write_seq(&dsi_ctx, 0x4f, 0x3c);
	m19_dcs_long_write_seq(&dsi_ctx, 0x53, 0x39);
	m19_dcs_long_write_seq(&dsi_ctx, 0x7b, 0x8f);
	m19_dcs_long_write_seq(&dsi_ctx, 0x7d, 0x04);
	m19_dcs_long_write_seq(&dsi_ctx, 0x80, 0x04);
	m19_dcs_long_write_seq(&dsi_ctx, 0x81, 0x04);
	m19_dcs_long_write_seq(&dsi_ctx, 0xa0, 0x0f);
	m19_dcs_long_write_seq(&dsi_ctx, 0xa2, 0x10);
	m19_dcs_long_write_seq(&dsi_ctx, 0xa4, 0x04);
	m19_dcs_long_write_seq(&dsi_ctx, 0xa5, 0x04);
	m19_dcs_long_write_seq(&dsi_ctx, 0xc6, 0xc0);
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0x25);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x18, 0x20);
	m19_dcs_long_write_seq(&dsi_ctx, 0xd7, 0x82);
	m19_dcs_long_write_seq(&dsi_ctx, 0xda, 0x02);
	m19_dcs_long_write_seq(&dsi_ctx, 0xdd, 0x02);
	m19_dcs_long_write_seq(&dsi_ctx, 0xe0, 0x02);
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0x26);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x80, 0x09);
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0x2a);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x20, 0x49);
	m19_dcs_long_write_seq(&dsi_ctx, 0x28, 0xae);
	m19_dcs_long_write_seq(&dsi_ctx, 0x29, 0x1a);
	m19_dcs_long_write_seq(&dsi_ctx, 0x2a, 0x6d);
	m19_dcs_long_write_seq(&dsi_ctx, 0x2f, 0x06);
	m19_dcs_long_write_seq(&dsi_ctx, 0x30, 0x1c);
	m19_dcs_long_write_seq(&dsi_ctx, 0x31, 0x60);
	m19_dcs_long_write_seq(&dsi_ctx, 0x33, 0x88);
	m19_dcs_long_write_seq(&dsi_ctx, 0x34, 0xb0);
	m19_dcs_long_write_seq(&dsi_ctx, 0x36, 0xbc);
	m19_dcs_long_write_seq(&dsi_ctx, 0x37, 0xab);
	m19_dcs_long_write_seq(&dsi_ctx, 0x39, 0xb8);
	m19_dcs_long_write_seq(&dsi_ctx, 0x3a, 0x1c);
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0x2b);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0xb7, 0x33);
	m19_dcs_long_write_seq(&dsi_ctx, 0xb8, 0x0e);
	m19_dcs_long_write_seq(&dsi_ctx, 0xc0, 0x02);
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0xc0);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x9c, 0x11);
	m19_dcs_long_write_seq(&dsi_ctx, 0x9d, 0x11);
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0xe0);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x35, 0x82);
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0xf0);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x1c, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x33, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x5a, 0x00);
	m19_dcs_long_write_seq(&dsi_ctx, 0x9c, 0x17);
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0x27);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x44, 0x00);
	m19_dcs_long_write_seq(&dsi_ctx, 0x40, 0x20);
	m19_dcs_long_write_seq(&dsi_ctx, 0x41, 0x30);
	m19_dcs_long_write_seq(&dsi_ctx, 0xff, 0x10);
	m19_dcs_long_write_seq(&dsi_ctx, 0xfb, 0x01);
	m19_dcs_long_write_seq(&dsi_ctx, 0x3b, 0x03, 0x14, 0x36, 0x04, 0x04);
	m19_dcs_long_write_seq(&dsi_ctx, 0x36, 0x00);
	m19_dcs_long_write_seq(&dsi_ctx, 0xb0, 0x00);
	m19_dcs_long_write_seq(&dsi_ctx, 0xc0, 0x03);
	m19_dcs_long_write_seq(&dsi_ctx, 0xc1, 0x89, 0x28, 0x00, 0x14, 0x00, 0xaa, 0x02, 0x0e, 0x00, 0x71, 0x00, 0x07, 0x05, 0x0e, 0x05, 0x16);
	m19_dcs_long_write_seq(&dsi_ctx, 0xc2, 0x1b, 0xa0);
	m19_dcs_long_write_seq(&dsi_ctx, 0x51, 0x07, 0xff);
	m19_dcs_long_write_seq(&dsi_ctx, 0x53, 0x2c);
	m19_dcs_long_write_seq(&dsi_ctx, 0x55, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x11);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x29);
	mipi_dsi_msleep(&dsi_ctx, 20);

	{
		u8 c1[16] = { 0 };
		int ret = mipi_dsi_dcs_read(dsi, 0xc1, c1, sizeof(c1));

		pr_info("sky-ddic: before PPS C1 ret=%d %*ph\n",
			ret, (int)sizeof(c1), c1);
	}
	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);
	pr_info("sky-panel: vendor-long-write init; PPS sent, err=%d\n",
		dsi_ctx.accum_err);
	{
		/* prove the DCS command path really reaches the DDIC */
		u8 id[3] = {0}, pm = 0;
		int r1 = mipi_dsi_dcs_read(dsi, 0x04, id, 3);   /* read display id */
		int r2 = mipi_dsi_dcs_read(dsi, 0x0a, &pm, 1); /* get power mode */
		pr_info("sky-panel: dcs read id=%d [%02x %02x %02x] pwr=%d [%02x]\n",
			r1, id[0], id[1], id[2], r2, pm);
	}
	{
		u8 c0 = 0, c1[16] = { 0 }, c2[2] = { 0 };
		int r0, r1, r2;

		r0 = mipi_dsi_dcs_read(dsi, 0xc0, &c0, sizeof(c0));
		r1 = mipi_dsi_dcs_read(dsi, 0xc1, c1, sizeof(c1));
		r2 = mipi_dsi_dcs_read(dsi, 0xc2, c2, sizeof(c2));
		pr_info("sky-ddic: C0 ret=%d %02x; C1 ret=%d %*ph; C2 ret=%d %*ph\n",
			r0, c0, r1, (int)sizeof(c1), c1,
			r2, (int)sizeof(c2), c2);
	}
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int m19_prepare(struct drm_panel *panel)
{
	struct m19_panel *ctx = container_of(panel, struct m19_panel, panel);
	int ret;

	if (ctx->prepared)
		return 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(panel->dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int m19_enable(struct drm_panel *panel)
{
	struct m19_panel *ctx = container_of(panel, struct m19_panel, panel);

	return m19_on(ctx);
}

static int m19_disable(struct drm_panel *panel)
{
	struct m19_panel *ctx = container_of(panel, struct m19_panel, panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int m19_unprepare(struct drm_panel *panel)
{
	struct m19_panel *ctx = container_of(panel, struct m19_panel, panel);

	if (!ctx->prepared)
		return 0;

	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	ctx->prepared = false;
	return 0;
}

static int m19_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &m19_mode);
	if (!mode)
		return -ENOMEM;

	mode->clock = clock_khz;
	drm_mode_set_name(mode);
	pr_info("sky-m19: mode %ux%u@%u clock=%u kHz\n", mode->hdisplay,
		mode->vdisplay, drm_mode_vrefresh(mode), mode->clock);
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = 69;
	connector->display_info.height_mm = 158;
	connector->display_info.bpc = 8;

	return 1;
}

static const struct drm_panel_funcs m19_funcs = {
	.prepare = m19_prepare,
	.enable = m19_enable,
	.disable = m19_disable,
	.unprepare = m19_unprepare,
	.get_modes = m19_get_modes,
};

static int m19_bl_update_status(struct backlight_device *bl)
{
	struct m19_panel *ctx = bl_get_data(bl);
	/* the panel uses an inverted 11-bit DBV value (see vendor DT) */
	u16 level = backlight_get_brightness(bl);
	u16 dbv = 2047 - level;

	return mipi_dsi_dcs_set_display_brightness(ctx->dsi, dbv);
}

static const struct backlight_ops m19_bl_ops = {
	.update_status = m19_bl_update_status,
};

static struct backlight_device *
m19_create_backlight(struct m19_panel *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 1023,
		.max_brightness = 2047,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
					      &m19_bl_ops, &props);
}

static int m19_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct m19_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct m19_panel, panel, &m19_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	pr_info("sky-m19: probe enter\n");
	ctx->supplies[0].supply = "vddio";
	ctx->supplies[1].supply = "avdd";
	ctx->supplies[2].supply = "ibb";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get supplies\n");

	pr_info("sky-m19: supplies ok\n");
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio), "failed to get reset gpio\n");

	pr_info("sky-m19: reset gpio %s\n", IS_ERR(ctx->reset_gpio) ? "ERR" : "ok");
	ctx->dsi = dsi;
	memcpy(&ctx->dsc, &m19_dsc, sizeof(ctx->dsc));
	ret = drm_dsc_compute_rc_parameters(&ctx->dsc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to compute DSC parameters\n");
	pr_info("sky-m19: dsc v%u.%u chunk=%u ixd=%u init_off=%u fl_bpg=%u model=%u edge=%u\n",
		ctx->dsc.dsc_version_major, ctx->dsc.dsc_version_minor,
		ctx->dsc.slice_chunk_size, ctx->dsc.initial_xmit_delay,
		ctx->dsc.initial_offset, ctx->dsc.first_line_bpg_offset,
		ctx->dsc.rc_model_size, ctx->dsc.rc_edge_factor);
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_NO_EOT_PACKET | MIPI_DSI_MODE_LPM |
			  MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT;
	dsi->dsc = &ctx->dsc;

	ctx->panel.prepare_prev_first = true;

	pr_info("sky-m19: dsi configured, creating backlight\n");
	ctx->bl = m19_create_backlight(ctx);
	if (IS_ERR(ctx->bl))
		return PTR_ERR(ctx->bl);

	pr_info("sky-m19: backlight %s\n", IS_ERR(ctx->bl) ? "ERR" : "ok");
	ret = drm_panel_of_backlight(&ctx->panel);
	pr_info("sky-m19: drm_panel_of_backlight ret=%d\n", ret);
	if (ret)
		return ret;

	pr_info("sky-m19: adding panel\n");
	ret = devm_drm_panel_add(dev, &ctx->panel);
	pr_info("sky-m19: panel add ret=%d\n", ret);
	if (ret)
		return ret;

	pr_info("sky-m19: attaching to dsi\n");
	ret = devm_mipi_dsi_attach(dev, dsi);
	pr_info("sky-m19: attach ret=%d\n", ret);
	return ret;
}

static void m19_remove(struct mipi_dsi_device *dsi)
{
	struct m19_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

}

static const struct of_device_id m19_of_match[] = {
	{ .compatible = "xiaomi,m19-36-02-0a-fhd-dsc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, m19_of_match);

static struct mipi_dsi_driver m19_driver = {
	.probe = m19_probe,
	.remove = m19_remove,
	.driver = {
		.name = "panel-xiaomi-m19",
		.of_match_table = m19_of_match,
	},
};
module_mipi_dsi_driver(m19_driver);

MODULE_DESCRIPTION("Xiaomi m19 (Redmi 12 5G) AMOLED panel driver");
MODULE_LICENSE("GPL");
