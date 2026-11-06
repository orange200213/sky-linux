// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Novatek NT36xxx series touchscreens
 *
 * Copyright (C) 2010 - 2018 Novatek, Inc.
 * Copyright (C) 2020 XiaoMi, Inc.
 * Copyright (C) 2020 AngeloGioacchino Del Regno <kholk11@gmail.com>
 * Copyright (C) 2023 99degree <www.github.com/99degree>
 *
 * Based on nt36xxx.c i2c driver from AngeloGioacchino Del Regno
 */

#include <asm/byteorder.h>
#include <drm/drm_panel.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/devm-helpers.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/irqnr.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/printk.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#include "nt36xxx.h"

/* Function prototype for _debug_irq */
void _debug_irq(struct nt36xxx_ts *ts, int line);

/* Main mmap to spi addr */
enum {
	MMAP_BASELINE_ADDR,
	MMAP_BASELINE_BTN_ADDR,
	MMAP_BLD_CRC_EN_ADDR,
	MMAP_BLD_DES_ADDR,
	MMAP_BLD_ILM_DLM_CRC_ADDR,
	MMAP_BLD_LENGTH_ADDR,
	MMAP_BOOT_RDY_ADDR,
	MMAP_DIFF_BTN_PIPE0_ADDR,
	MMAP_DIFF_BTN_PIPE1_ADDR,
	MMAP_DIFF_PIPE0_ADDR,
	MMAP_DIFF_PIPE1_ADDR,
	MMAP_DLM_DES_ADDR,
	MMAP_DLM_LENGTH_ADDR,
	MMAP_DMA_CRC_EN_ADDR,
	MMAP_DMA_CRC_FLAG_ADDR,
	MMAP_ENG_RST_ADDR,
	MMAP_EVENT_BUF_ADDR,
	MMAP_G_DLM_CHECKSUM_ADDR,
	MMAP_G_ILM_CHECKSUM_ADDR,
	MMAP_ILM_DES_ADDR,
	MMAP_ILM_LENGTH_ADDR,
	MMAP_POR_CD_ADDR,
	MMAP_RAW_BTN_PIPE0_ADDR,
	MMAP_RAW_BTN_PIPE1_ADDR,
	MMAP_RAW_PIPE0_ADDR,
	MMAP_RAW_PIPE1_ADDR,
	MMAP_READ_FLASH_CHECKSUM_ADDR,
	MMAP_RW_FLASH_DATA_ADDR,
	MMAP_R_DLM_CHECKSUM_ADDR,
	MMAP_R_ILM_CHECKSUM_ADDR,
	MMAP_SPI_RD_FAST_ADDR,
	MMAP_SWRST_N8_ADDR,
	MMAP_HISTORY_EVENT0,
	MMAP_HISTORY_EVENT1,

	/* below are magic numbers in source code */
	MMAP_MAGIC_NUMBER_0X1F64E_ADDR,

	/* this addr is not specific to */
	MMAP_TOP_ADDR,
	MMAP_MAX_ADDR = MMAP_TOP_ADDR,
} nt36xxx_ts_mem_map;

static struct drm_panel_follower_funcs nt36xxx_panel_follower_funcs;

/* Bounded firmware re-init: the vendor WDT recovery threshold and a cap. */
#define NT36XXX_FW_RESET_LIMIT	10
#define NT36XXX_RECOVER_LIMIT	5
#define NT36XXX_RECOVER_FAST_MS	2000
#define NT36XXX_RECOVER_SLOW_MS	60000
#define NT36XXX_RECOVER_SLOW_TRIES 3
#define NT36XXX_HEALTH_PERIOD_MS 2000
#define NT36XXX_FIRST_INIT_MS	14000

static int sky_fw_attempts;

struct nt36xxx_ts {
	struct regmap *regmap;

	struct input_dev *input;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *irq_gpio;
	int irq;
	struct device *dev;

	struct mutex lock;

#define NT36XXX_STATUS_SUSPEND			BIT(0)
#define NT36XXX_STATUS_DOWNLOAD_COMPLETE	BIT(1)
#define NT36XXX_STATUS_DOWNLOAD_RECOVER		BIT(2)
	unsigned int status;

	struct touchscreen_properties prop;
	struct nt36xxx_fw_info fw_info;
	struct nt36xxx_abs_object abs_obj;

	struct drm_panel_follower panel_follower;

	struct delayed_work work;
	struct delayed_work health_work;

	/* firmware-reset handling: bounded, rate limited re-init */
	unsigned int fw_reset_cnt;
	unsigned int recover_tries;
	unsigned long recover_time;
	unsigned int health_bad;
	unsigned int report_log_cnt;
	bool quiet_init;
	bool give_up;
	bool init_once;

	/* this is a duplicate with nt36xxx_chip_data and since the address might
	 * change in boot/init/download stages so make it a copy of initial map and
	 * update accordingly
	 */
	u32 *mmap;
	u32 mmap_data[MMAP_MAX_ADDR];

	struct nvt_fw_parse_data fw_data;
	struct nvt_ts_bin_map *bin_map;

	uint8_t hw_crc;

	struct firmware fw_entry; /* containing request fw data */
	u8 *fw_raw;              /* firmware image loaded in probe, DMA-capable */
	size_t fw_raw_size;
	const char *firmware_path;
	const struct nt36xxx_chip_data *data;
};

static const struct nt36xxx_trim_table trim_id_table[] = {
#if 0
	/* TODO: port and test all related module */
	{
		.id = { 0x0A, 0xFF, 0xFF, 0x72, 0x66, 0x03 },
		.mask = { 1, 0, 0, 1, 1, 1 },
		.mapid = NT36672A_IC,
	},
	{
		.id = { 0x55, 0x00, 0xFF, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0x55, 0x72, 0xFF, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xAA, 0x00, 0xFF, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xAA, 0x72, 0xFF, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x72, 0x67, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x70, 0x66, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x70, 0x67, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x72, 0x66, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x25, 0x65, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x70, 0x68, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x76, 0x66, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36676F_IC,
	},
#endif
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x75, 0x66, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36675_IC,
		.hw_crc = 2,  /* 3Bytes */
	},
	{
		.id = { 0x0C, 0xFF, 0xFF, 0x72, 0x66, 0x03 },
		.mask = { 1, 0, 0, 1, 1, 1 },
		.mapid = NT36675_IC,
		.hw_crc = 2,  /* 3Bytes */
	},
	{ },
};

const u32 nt36675_memory_maps[] = {
	[MMAP_EVENT_BUF_ADDR] = 0x22D00,
	[MMAP_RAW_PIPE0_ADDR] = 0x24000,
	[MMAP_RAW_PIPE1_ADDR] = 0x24000,
	[MMAP_BASELINE_ADDR] = 0x21B90,
	[MMAP_DIFF_PIPE0_ADDR] = 0x20C60,
	[MMAP_DIFF_PIPE1_ADDR] = 0x24C60,
	[MMAP_READ_FLASH_CHECKSUM_ADDR] = 0x24000,
	[MMAP_RW_FLASH_DATA_ADDR] = 0x24002,
	[MMAP_BOOT_RDY_ADDR] = 0x3F10D,
	[MMAP_BLD_LENGTH_ADDR] = 0x3F138,
	[MMAP_ILM_LENGTH_ADDR] = 0x3F118,
	[MMAP_DLM_LENGTH_ADDR] = 0x3F130,
	[MMAP_BLD_DES_ADDR] = 0x3F114,
	[MMAP_ILM_DES_ADDR] = 0x3F128,
	[MMAP_DLM_DES_ADDR] = 0x3F12C,
	[MMAP_G_ILM_CHECKSUM_ADDR] = 0x3F100,
	[MMAP_G_DLM_CHECKSUM_ADDR] = 0x3F104,
	[MMAP_R_ILM_CHECKSUM_ADDR] = 0x3F120,
	[MMAP_R_DLM_CHECKSUM_ADDR] = 0x3F124,
	[MMAP_BLD_CRC_EN_ADDR] = 0x3F30E,
	[MMAP_DMA_CRC_EN_ADDR] = 0x3F136,
	[MMAP_BLD_ILM_DLM_CRC_ADDR] = 0x3F133,
	[MMAP_DMA_CRC_FLAG_ADDR] = 0x3F134,

	/* below are specified by dts), so it might change by project-based */
	[MMAP_SPI_RD_FAST_ADDR] = 0x03F310,
	[MMAP_SWRST_N8_ADDR] = 0x03F0FE,

	[MMAP_HISTORY_EVENT0] = 0x23D10,
	[MMAP_HISTORY_EVENT1] = 0x23D50,

	[MMAP_ENG_RST_ADDR] = 0x7FFF80,
	[MMAP_MAGIC_NUMBER_0X1F64E_ADDR] = 0x1F64E,

	[MMAP_TOP_ADDR] = 0xffffff,
};

/*
 * NT36672A runtime map (vendor NT36672A_memory_map).  Kept for reference and
 * for future chips whose trim id selects it; the trim id of this device
 * (0x0C,0xFF,0xFF,0x72,0x66,0x03) selects NT36675 (mapid 68), exactly as the
 * vendor trim_id_table does, so this table is NOT used here.
 */
const u32 nt36672a_memory_maps[] = {
	[MMAP_EVENT_BUF_ADDR] = 0x21C00,
	[MMAP_RAW_PIPE0_ADDR] = 0x20000,
	[MMAP_RAW_PIPE1_ADDR] = 0x23000,
	[MMAP_BASELINE_ADDR] = 0x20BFC,
	[MMAP_BASELINE_BTN_ADDR] = 0x23BFC,
	[MMAP_DIFF_PIPE0_ADDR] = 0x206DC,
	[MMAP_DIFF_PIPE1_ADDR] = 0x236DC,
	[MMAP_RAW_BTN_PIPE0_ADDR] = 0x20510,
	[MMAP_RAW_BTN_PIPE1_ADDR] = 0x23510,
	[MMAP_DIFF_BTN_PIPE0_ADDR] = 0x20BF0,
	[MMAP_DIFF_BTN_PIPE1_ADDR] = 0x23BF0,
	[MMAP_READ_FLASH_CHECKSUM_ADDR] = 0x24000,
	[MMAP_RW_FLASH_DATA_ADDR] = 0x24002,
	[MMAP_BOOT_RDY_ADDR] = 0x3F10D,
	[MMAP_BLD_LENGTH_ADDR] = 0x3F10E,
	[MMAP_ILM_LENGTH_ADDR] = 0x3F118,
	[MMAP_DLM_LENGTH_ADDR] = 0x3F130,
	[MMAP_BLD_DES_ADDR] = 0x3F114,
	[MMAP_ILM_DES_ADDR] = 0x3F128,
	[MMAP_DLM_DES_ADDR] = 0x3F12C,
	[MMAP_G_ILM_CHECKSUM_ADDR] = 0x3F100,
	[MMAP_G_DLM_CHECKSUM_ADDR] = 0x3F104,
	[MMAP_R_ILM_CHECKSUM_ADDR] = 0x3F120,
	[MMAP_R_DLM_CHECKSUM_ADDR] = 0x3F124,
	[MMAP_BLD_CRC_EN_ADDR] = 0x3F30E,
	[MMAP_DMA_CRC_EN_ADDR] = 0x3F132,
	[MMAP_BLD_ILM_DLM_CRC_ADDR] = 0x3F133,
	[MMAP_DMA_CRC_FLAG_ADDR] = 0x3F134,

	[MMAP_SPI_RD_FAST_ADDR] = 0x03F310,
	[MMAP_SWRST_N8_ADDR] = 0x03F0FE,

	[MMAP_HISTORY_EVENT0] = 0x218D6,
	[MMAP_HISTORY_EVENT1] = 0x24B2D,

	[MMAP_ENG_RST_ADDR] = 0x7FFF80,
	[MMAP_MAGIC_NUMBER_0X1F64E_ADDR] = 0x1F64E,

	[MMAP_TOP_ADDR] = 0xffffff,
};

void _debug_irq(struct nt36xxx_ts *ts, int line){
	struct irq_desc *desc;
	desc = irq_data_to_desc( irq_get_irq_data(ts->irq));
	dev_info(ts->dev, "%d irq_desc depth=%d", line, desc->depth );
}

#define debug_irq(a) _debug_irq(a, __LINE__)

static int nt36xxx_eng_reset_idle(struct nt36xxx_ts *ts)
{
	int ret;

	if(!ts) {
		dev_err(ts->dev, "%s %s empty", __func__, "nt36xxx_ts");
		return -EINVAL;
	}

	if(!ts->mmap) {
		dev_err(ts->dev, "%s %s empty", __func__, "ts->mmap");
		return -EINVAL;
	}

	if(ts->mmap[MMAP_ENG_RST_ADDR] == 0) {
		dev_err(ts->dev, "%s %s empty", __func__, "MMAP_ENG_RST_ADDR");
		return -EINVAL;
	}

	/* HACK to output something without read */
	ret = regmap_write(ts->regmap, ts->mmap[MMAP_ENG_RST_ADDR],
					   0x5a);
	if (ret) {
		dev_err(ts->dev, "%s regmap write error\n", __func__);
		return ret;
	}

	/* Wait until the MCU resets the fw state */
	usleep_range(15000, 16000);

	/* seemed not long enough */
	msleep(30);
	return ret;
}

/**
 * nt36xxx_sw_reset_idle - MCU idle command (vendor nvt_sw_reset_idle)
 * @ts: Main driver structure
 *
 * The retail driver's WDT recovery sends 0xAA to SWRST_SIF_ADDR when the
 * report area reads 0xFE, before it re-downloads the firmware.  This is the
 * only reset primitive the retail build uses: it has no hardware reset pin
 * (NVT_TOUCH_SUPPORT_HW_RST=0, GPIO12 is unclaimed in its pinctrl state).
 */
static void nt36xxx_sw_reset_idle(struct nt36xxx_ts *ts)
{
	regmap_write(ts->regmap, ts->mmap[MMAP_SWRST_N8_ADDR],
		     NT36XXX_CMD_SW_RESET_IDLE);
	msleep(15);
}

/**
 * nt36xxx_sw_reset - MCU software reset (vendor nvt_sw_reset)
 * @ts: Main driver structure
 */
static void nt36xxx_sw_reset(struct nt36xxx_ts *ts)
{
	regmap_write(ts->regmap, ts->mmap[MMAP_SWRST_N8_ADDR],
		     NT36XXX_CMD_SW_RESET);
	msleep(10);
}

/**
 * nt36xxx_sky_dump_fw_history - Log the IC's crash history registers
 * @ts: Main driver structure
 *
 * The retail driver prints these two regions before it re-downloads the
 * firmware (nvt_read_fw_history).  They are the only record of why the
 * firmware stopped, so keep them for the recovery path only.
 */
static void nt36xxx_sky_dump_fw_history(struct nt36xxx_ts *ts)
{
	static const int idx[] = { MMAP_HISTORY_EVENT0, MMAP_HISTORY_EVENT1 };
	u8 buf[16];
	int i;

	for (i = 0; i < ARRAY_SIZE(idx); i++) {
		if (!ts->mmap[idx[i]])
			continue;
		if (regmap_raw_read(ts->regmap, ts->mmap[idx[i]], buf,
				    sizeof(buf)))
			continue;
		dev_info(ts->dev, "sky-fwhist[%d]: %16phN\n", i, buf);
	}
}

/**
 * nt36xxx_sky_recover_reset - Bring a wedged MCU back, retail-driver style
 * @ts: Main driver structure
 *
 * Order taken from the retail driver: idle command (0xAA), engineering reset
 * (0x5A), and the caller then runs the bootloader reset (0x69) and the normal
 * download.  No reset GPIO is involved - the board does not use one.
 */
static void nt36xxx_sky_recover_reset(struct nt36xxx_ts *ts)
{
	nt36xxx_sw_reset_idle(ts);
	nt36xxx_eng_reset_idle(ts);
	nt36xxx_sw_reset(ts);
}

/**
 * nt36xxx_bootloader_reset - Reset MCU to bootloader
 * @ts: Main driver structure
 *
 * Return: Always zero for success, negative number for error
 */
static int nt36xxx_bootloader_reset(struct nt36xxx_ts *ts)
{
	int ret = 0;

	//in spi version, need to set page to SWRST_N8_ADDR
	if (ts->mmap[MMAP_SWRST_N8_ADDR]) {
		ret = regmap_write(ts->regmap, ts->mmap[MMAP_SWRST_N8_ADDR],
			   NT36XXX_CMD_BOOTLOADER_RESET);
		if (ret)
			return ret;
	} else {
		pr_info("plz make sure MMAP_SWRST_N8_ADDR is set!\n");
		return -EINVAL;
	}

	/* MCU has to reboot from bootloader: this is the typical boot time */
	msleep(35);

	if (ts->mmap[MMAP_SPI_RD_FAST_ADDR]) {
		ret = regmap_write(ts->regmap, ts->mmap[MMAP_SPI_RD_FAST_ADDR], 0);
		if (ret)
			return ret;
	}

	return ret;
}

/* read SRAM back and compare with the firmware image (diagnostics only) */
static void __maybe_unused nt36xxx_sky_verify(struct nt36xxx_ts *ts, int idx, const char *when)
{
	const u32 chunk = 4096;
	u32 off, size, bad_chunks = 0, first_bad = 0xffffffff;
	u8 want_b, got_b = 0;
	u8 *buf;
	int ret;

	if (!ts->bin_map || !ts->fw_entry.data || !ts->bin_map[idx].size)
		return;

	size = ts->bin_map[idx].size + 1;
	buf = kmalloc(chunk, GFP_KERNEL);
	if (!buf)
		return;

	for (off = 0; off < size; off += chunk) {
		u32 n = min(chunk, size - off);
		u32 k;

		ret = regmap_raw_read(ts->regmap, ts->bin_map[idx].sram_addr + off, buf, n);
		if (ret) {
			dev_err(ts->dev, "sky-verify[%s] %s read failed at off=0x%x ret=%d\n",
				when, ts->bin_map[idx].name, off, ret);
			break;
		}
		if (memcmp(buf, ts->fw_entry.data + ts->bin_map[idx].bin_addr + off, n)) {
			u32 diffs = 0;

			for (k = 0; k < n; k++) {
				if (buf[k] != ts->fw_entry.data[ts->bin_map[idx].bin_addr + off + k]) {
					diffs++;
					if (first_bad == 0xffffffff) {
						first_bad = off + k;
						want_b = ts->fw_entry.data[ts->bin_map[idx].bin_addr + off + k];
						got_b = buf[k];
					}
				}
			}
			bad_chunks++;
			dev_info(ts->dev, "sky-verify[%s] %s bad off=0x%x diffs=%u/%u\n",
				 when, ts->bin_map[idx].name, off, diffs, n);
			if (first_bad != 0xffffffff && off <= first_bad && first_bad < off + n) {
				u32 m = first_bad - off;
				u32 lim = min((u32)8, n - m);

				dev_info(ts->dev, "sky-verify[%s]  got : %*phN\n",
					 when, lim * 2, buf + m);
				dev_info(ts->dev, "sky-verify[%s]  want: %*phN\n",
					 when, lim * 2, ts->fw_entry.data + ts->bin_map[idx].bin_addr + first_bad);
				/* test a few obvious shift hypotheses over 256 B */
				{
					static const int shifts[] = { 128, -128, 4096, -4096, 8192, -8192, 16384, -16384 };
					int si;

					for (si = 0; si < ARRAY_SIZE(shifts); si++) {
						long so = (long)first_bad + shifts[si];
						u32 cmplen = 256;

						if (so < 1 || so + cmplen >= (long)size)
							continue;
						if (!memcmp(ts->fw_entry.data + ts->bin_map[idx].bin_addr + so,
							    ts->fw_entry.data + ts->bin_map[idx].bin_addr + first_bad,
							    cmplen))
							dev_info(ts->dev, "sky-verify[%s]  shift %+d looks duplicated\n",
								 when, shifts[si]);
					}
					/* does got[m..] match want shifted by +/- k inside the same chunk? */
					for (k = 1; k < 64; k++) {
						if (m + k + 32 > n)
							break;
						if (!memcmp(buf + m + k,
							    ts->fw_entry.data + ts->bin_map[idx].bin_addr + first_bad,
							    32)) {
							dev_info(ts->dev, "sky-verify[%s]  got is want delayed by %u\n",
								 when, k);
							break;
						}
					}
				}
			}
		}
	}
	kfree(buf);
	dev_info(ts->dev, "sky-verify[%s] %s size=0x%x bad_chunks=%u/%u first_bad=0x%x got=0x%02x want=0x%02x\n",
		 when, ts->bin_map[idx].name, size, bad_chunks,
		 (size + chunk - 1) / chunk, first_bad, got_b, want_b);
}

/* read-only hardware CRC diagnostics (port of vendor nvt_read_bld_hw_crc) */
static u32 nt36xxx_read_u32(struct nt36xxx_ts *ts, u32 addr)
{
	u8 b[4] = { 0 };

	if (regmap_raw_read(ts->regmap, addr, b, sizeof(b)))
		return 0xdeadbeef;

	return b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24;
}

static void nt36xxx_sky_dump_crc(struct nt36xxx_ts *ts)
{
	u8 flag = 0;

	/* repeated recovery attempts must not flood the ring buffer */
	if (!ts->bin_map || ts->quiet_init)
		return;

	regmap_raw_read(ts->regmap, ts->mmap[MMAP_BLD_ILM_DLM_CRC_ADDR], &flag, 1);
	dev_info(ts->dev, "sky-crc: flag=0x%02x (done=%d ilm=%d dlm=%d)\n",
		 flag, !!(flag & 4), !!(flag & 1), !!(flag & 2));
	dev_info(ts->dev, "sky-crc: ILM bin=0x%08x golden=0x%08x result=0x%08x\n",
		 ts->bin_map[0].crc,
		 nt36xxx_read_u32(ts, ts->mmap[MMAP_G_ILM_CHECKSUM_ADDR]),
		 nt36xxx_read_u32(ts, ts->mmap[MMAP_R_ILM_CHECKSUM_ADDR]));
	if (ts->fw_data.ilm_dlm_num > 1)
		dev_info(ts->dev, "sky-crc: DLM bin=0x%08x golden=0x%08x result=0x%08x\n",
			 ts->bin_map[1].crc,
			 nt36xxx_read_u32(ts, ts->mmap[MMAP_G_DLM_CHECKSUM_ADDR]),
			 nt36xxx_read_u32(ts, ts->mmap[MMAP_R_DLM_CHECKSUM_ADDR]));
}

/**
 * nt36xxx_check_reset_state - Check the boot state during reset
 * @ts: Main driver structure
 * @fw_state: Enumeration containing firmware states
 *
 * Return: Always zero for success, negative number for error
 */
static int nt36xxx_check_reset_state(struct nt36xxx_ts *ts,
				     enum nt36xxx_fw_state fw_state)
{
	u8 buf[8] = { 0 };
	u8 last = 0xff, last2 = 0xff;
	int ret = 0, retry = NT36XXX_MAX_FW_RST_RETRY;

	do {
		ret = regmap_raw_read(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR]
				 | NT36XXX_EVT_RESET_COMPLETE, buf, 6);
		/*
		 * This driver's read helper puts the first data byte in buf[0]
		 * (see nt36xxx_chip_version_init), while the vendor code it was
		 * ported from reads into buf[1] onwards.  The state byte is the
		 * first byte after the address, i.e. buf[0].
		 */
		if (likely(ret == 0) &&
		    (buf[0] >= fw_state) &&
		    (buf[0] <= NT36XXX_STATE_MAX)) {
			ret = 0;
			break;
		}
		if (!ts->quiet_init && (buf[0] != last || buf[1] != last2)) {
			dev_info(ts->dev, "sky-state: raw=%*phN (ret=%d, poll %d)\n",
				 6, buf, ret, NT36XXX_MAX_FW_RST_RETRY - retry);
			last = buf[0];
			last2 = buf[1];
		}
		usleep_range(10000, 11000);
	} while (--retry);

	if (!retry) {
		dev_err(ts->dev, "Firmware reset failed (raw=%*phN want 0x%02x..0x%02x ret=%d).\n",
			6, buf, fw_state, NT36XXX_STATE_MAX, ret);
		ret = -EBUSY;
	}

	return ret;
}

/**
 * nt36xxx_scale_coord - Convert a raw report coordinate to a panel pixel
 * @raw: Raw coordinate as read from the report area
 * @raw_max: Full scale of the raw coordinate
 * @px_max: Highest valid panel pixel coordinate
 *
 * Return: @raw mapped linearly onto 0..@px_max, clamped at @px_max
 */
static u16 nt36xxx_scale_coord(u16 raw, unsigned int raw_max, u16 px_max)
{
	u32 scaled = (u32)raw * (px_max + 1) / raw_max;

	return scaled > px_max ? px_max : scaled;
}

/**
 * nt36xxx_report_is_fw_reset - Detect the "firmware is not running" report
 * @point: First bytes of the report area
 *
 * The vendor recovery code looks at the six bytes of the first point and
 * distinguishes two non-report patterns: all-0x77 is the firmware's ESD
 * handshake marker (this IC also idles with it, so it must be ignored), while
 * an all-0xFD or all-0xFE window means the firmware has reset and is not
 * scanning.  Bytes 0..5 here are the vendor's bytes 1..6: this transport
 * already drops the two read-dummy bytes.
 *
 * Return: true when the report area holds the firmware-reset pattern
 */
static bool nt36xxx_report_is_fw_reset(const u8 *point)
{
	int i;

	if (point[0] != 0xFD && point[0] != 0xFE)
		return false;

	for (i = 1; i < 6; i++) {
		if (point[i] != point[0])
			return false;
	}

	return true;
}

static bool nt36xxx_report_is_live(const u8 *point)
{
	int i;

	/* 0x77 is this firmware's valid idle frame. */
	if (point[0] == 0x77)
		return true;

	/* A real point has a non-uniform six-byte record. */
	for (i = 1; i < 6; i++) {
		if (point[i] != point[0])
			return true;
	}

	return point[0] != 0xff && point[0] != 0xfd && point[0] != 0xfe;
}

static void nt36xxx_sky_schedule_recover(struct nt36xxx_ts *ts, const char *why);

/**
 * nt36xxx_wait_first_report - Wait for a live report area before enabling IRQ
 * @ts: Main driver structure
 *
 * ReK may be reported before the first frame is written. The transition can
 * briefly read as all 0xff/0xfe; keep IRQ masked until a live frame appears.
 */
static bool nt36xxx_wait_first_report(struct nt36xxx_ts *ts)
{
	u8 point[8] = { 0 };
	int i;

	for (i = 0; i < 100; i++) {
		if (regmap_raw_read(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR],
				    point, sizeof(point)))
			return false;
		if (nt36xxx_report_is_live(point))
			return true;
		usleep_range(5000, 6000);
	}

	dev_err(ts->dev, "sky: report area did not reach a live frame\n");
	return false;
}

/**
 * nt36xxx_report - Report touch events
 * @ts: Main driver structure
 *
 * Return: Always zero for success, negative number for error
 */
static void nt36xxx_report(struct nt36xxx_ts *ts)
{
	struct nt36xxx_abs_object *obj = &ts->abs_obj;
	struct input_dev *input = ts->input;
	unsigned int raw_x_max = ts->data->abs_x_max * NT36XXX_RAW_UNITS_PER_PX;
	unsigned int raw_y_max = ts->data->abs_y_max * NT36XXX_RAW_UNITS_PER_PX;
	u8 input_id = 0;
	u8 point[POINT_DATA_LEN + 1] = { 0 };
	unsigned int ppos = 0;
	int i, ret;

	/* the init worker owns the IC until its first download completed */
	if (!(ts->status & NT36XXX_STATUS_DOWNLOAD_COMPLETE) || ts->give_up)
		return;

	/*
	 * SPI has no arbitration: a report read that interleaves with another
	 * user of the bus (the health poll, an init sequence) puts the page
	 * select and the data phase out of order and can wedge the firmware.
	 * The vendor serialises every access with this lock; so do we.
	 */
	mutex_lock(&ts->lock);

	ret = regmap_raw_read(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR],
				point, sizeof(point));
	if (ret < 0) {
		dev_err(ts->dev,
			"Cannot read touch point data: %d\n", ret);
		goto xfer_error;
	}

	/*
	 * A firmware that has reset keeps the interrupt line busy at the scan
	 * rate while every report read returns 0xFE, so the driver has to
	 * re-init the IC itself; the vendor does this after ten reads.
	 */
	if (nt36xxx_report_is_fw_reset(point)) {
		if (++ts->fw_reset_cnt >= NT36XXX_FW_RESET_LIMIT)
			nt36xxx_sky_schedule_recover(ts, "reports all 0xFD/0xFE");
	} else {
		ts->fw_reset_cnt = 0;
	}

	/* bounded: the first reads after every init, with the IC's own state */
	if (ts->report_log_cnt < 3) {
		u8 state[6] = { 0 };
		int level = -1;

		regmap_raw_read(ts->regmap,
				ts->mmap[MMAP_EVENT_BUF_ADDR] |
						NT36XXX_EVT_RESET_COMPLETE,
				state, 6);
		if (ts->irq_gpio)
			level = gpiod_get_raw_value_cansleep(ts->irq_gpio);

		dev_info(ts->dev, "sky-report[%u]: %6phN state=%6phN irq_line=%d\n",
			 ts->report_log_cnt, point, state, level);
	}
	ts->report_log_cnt++;

	for (i = 0; i < TOUCH_MAX_FINGER_NUM; i++) {
		ppos = 6 * i;
		input_id = point[ppos + 0] >> 3;

		if ((input_id == 0) || (input_id > TOUCH_MAX_FINGER_NUM)) {
			continue;
		}

		if (((point[ppos] & 0x07) == 0x01) ||
		    ((point[ppos] & 0x07) == 0x02)) {
			u16 raw_x = (point[ppos + 1] << 8) | point[ppos + 2];
			u16 raw_y = (point[ppos + 3] << 8) | point[ppos + 4];

			obj->x = nt36xxx_scale_coord(raw_x, raw_x_max,
						     ts->prop.max_x);
			obj->y = nt36xxx_scale_coord(raw_y, raw_y_max,
						     ts->prop.max_y);

			/*
			 * The retail "super resolution" pressure lives at
			 * 0x22D00 + 98, which on this firmware is the event
			 * buffer's RESET_COMPLETE register, not pressure (proven
			 * by the sky-report pres= dump).  The only per-point
			 * strength this IC reports is the last point byte, so
			 * both the touch major and the pressure come from it.
			 */
			obj->tm = point[ppos + 5];
			if (obj->tm == 0)
				obj->tm = 1;

			obj->z = obj->tm;


			input_mt_slot(input, input_id - 1);
			input_mt_report_slot_state(input,
						   MT_TOOL_FINGER, true);
			touchscreen_report_pos(input, &ts->prop,
						obj->x,
						obj->y, true);

			input_report_abs(input, ABS_MT_TOUCH_MAJOR, obj->tm);
			input_report_abs(input, ABS_MT_PRESSURE, obj->z);
		}
	}

	input_mt_sync_frame(input);

	input_sync(input);

xfer_error:
	mutex_unlock(&ts->lock);
}

static irqreturn_t nt36xxx_irq_handler(int irq, void *dev_id)
{
	struct nt36xxx_ts *ts = dev_id;

	if (!ts->mmap)
		goto exit;

	disable_irq_nosync(ts->irq);

	nt36xxx_report(ts);

	enable_irq(ts->irq);

exit:
	if (ts->status & NT36XXX_STATUS_DOWNLOAD_RECOVER) {
		ts->status &= ~NT36XXX_STATUS_DOWNLOAD_RECOVER;
		schedule_delayed_work(&ts->work, 40000);
	}

	return IRQ_HANDLED;
}


/**
 * nt36xxx_chip_version_init - Detect Novatek NT36xxx family IC
 * @ts: Main driver structure
 *
 * This function reads the ChipID from the IC and sets the right
 * memory map for the detected chip.
 *
 * Return: Always zero for success, negative number for error
 */
static int nt36xxx_chip_version_init(struct nt36xxx_ts *ts)
{
	u8 buf[32] = { 0 };
	int retry = NT36XXX_MAX_RETRIES;
	int sz = sizeof(trim_id_table) / sizeof(struct nt36xxx_trim_table);
	int i, list, mapid, ret;

	ret = nt36xxx_bootloader_reset(ts);
	if (ret) {
		dev_err(ts->dev, "Can't reset the nvt IC\n");
		return ret;
	}

	do {
		ret = regmap_raw_read(ts->regmap, ts->mmap[MMAP_MAGIC_NUMBER_0X1F64E_ADDR], buf, 7);

		if (ret)
			continue;

		if (!ts->quiet_init)
			dev_info(ts->dev, "%s buf[0]=0x%02X, buf[1]=0x%02X, buf[2]=0x%02X, buf[3]=0x%02X, buf[4]=0x%02X, buf[5]=0x%02X, buf[6]=0x%02X sz=%d\n",
				__func__, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], sz);

		/* Compare read chip id with trim list */
		for (list = 0; list < sz; list++) {
			if (!trim_id_table[list].mapid)
				continue;

			/* Compare each not masked byte */
			for (i = 0; i < NT36XXX_ID_LEN_MAX; i++) {
				if (trim_id_table[list].mask[i] &&
				    buf[i] != trim_id_table[list].id[i])
					break;
			}

			/* found and match with mask */
			if (i == NT36XXX_ID_LEN_MAX) {
				mapid = trim_id_table[list].mapid;
				ret = 0;
				ts->hw_crc = trim_id_table[list].hw_crc;

				if (mapid == 0) {
					dev_info(ts->dev, "NVT touch IC hw not found i=%d list=%d\n", i, list);
					ret = -ENOENT;
					goto exit;
				}

				WARN_ON(ts->hw_crc < 1);

				dev_dbg(ts->dev, "hw crc support=%d\n", ts->hw_crc);

				dev_info(ts->dev, "This is NVT touch IC, %x, mapid %d", *(int*)&buf[4], mapid);
				return 0;
			}

			ret = -ENOENT;
		}

		usleep_range(10000, 11000);
	} while (--retry);

exit:
	return ret;
}

/*
 * this function is nearly direct copy from vendor source
*/
static int32_t nvt_bin_header_parser(struct device *dev, int hw_crc, const u8 *fwdata, size_t fwsize, struct nvt_ts_bin_map **bin_map_ptr, uint8_t *partition_ptr, uint8_t ilm_dlm_num)
{
	uint8_t list = 0;
	uint32_t pos = 0x00;
	uint32_t end = 0x00;
	uint8_t info_sec_num = 0;
	uint8_t ovly_sec_num = 0;
	uint8_t ovly_info = 0;
	uint8_t partition;
	struct nvt_ts_bin_map *bin_map;

	/* Find the header size */
	end = fwdata[0] + (fwdata[1] << 8) + (fwdata[2] << 16) + (fwdata[3] << 24);
	pos = 0x30;	// info section start at 0x30 offset
	while (pos < end) {
		info_sec_num ++;
		pos += 0x10;	/* each header info is 16 bytes */
	}

	/*
	 * Find the DLM OVLY section
	 * [0:3] Overlay Section Number
	 * [4]   Overlay Info
	 */
	ovly_info = (fwdata[0x28] & 0x10) >> 4;
	ovly_sec_num = (ovly_info) ? (fwdata[0x28] & 0x0F) : 0;

	/*
	 * calculate all partition number
	 * ilm_dlm_num (ILM & DLM) + ovly_sec_num + info_sec_num
	 */
	*partition_ptr = partition = ilm_dlm_num + ovly_sec_num + info_sec_num;
	dev_dbg(dev, "ovly_info = %d, ilm_dlm_num = %d, ovly_sec_num = %d, info_sec_num = %d, partition = %d\n",
			ovly_info, ilm_dlm_num, ovly_sec_num, info_sec_num, partition);

	/* allocated memory for header info */
	*bin_map_ptr = bin_map = (struct nvt_ts_bin_map *)devm_kzalloc(dev, (partition + 1) * sizeof(struct nvt_ts_bin_map), GFP_KERNEL);
	if(bin_map == NULL) {
		dev_err(dev, "kzalloc for bin_map failed!\n");
		return -ENOMEM;
	}


	for (list = 0; list < partition; list++) {
		/*
		 * [1] parsing ILM & DLM header info
		 * bin_addr : sram_addr : size (12-bytes)
		 * crc located at 0x18 & 0x1C
		 */
		if (list < ilm_dlm_num) {
			memcpy(&bin_map[list].bin_addr, &(fwdata[0 + list*12]), 4);
			memcpy(&bin_map[list].sram_addr, &(fwdata[4 + list*12]), 4);
			memcpy(&bin_map[list].size, &(fwdata[8 + list*12]), 4);
			memcpy(&bin_map[list].crc, &(fwdata[0x18 + list*4]), 4);

			if (!hw_crc) {
				dev_err(dev, "%s %d sw-crc not support", __func__, __LINE__);
				return -EINVAL;
			}

			if (list == 0)
				sprintf(bin_map[list].name, "ILM");
			else if (list == 1)
				sprintf(bin_map[list].name, "DLM");
		}

		/*
		 * [2] parsing others header info
		 * sram_addr : size : bin_addr : crc (16-bytes)
		 */
		if ((list >= ilm_dlm_num) && (list < (ilm_dlm_num + info_sec_num))) {

			/* others partition located at 0x30 offset */
			pos = 0x30 + (0x10 * (list - ilm_dlm_num));

			memcpy(&bin_map[list].sram_addr, &(fwdata[pos]), 4);
			memcpy(&bin_map[list].size, &(fwdata[pos+4]), 4);
			memcpy(&bin_map[list].bin_addr, &(fwdata[pos+8]), 4);
			memcpy(&bin_map[list].crc, &(fwdata[pos+12]), 4);

			if (!hw_crc) {
				dev_info(dev, "ok, hw_crc not presents!");
				return -EINVAL;
			}

			/* detect header end to protect parser function */
			if ((bin_map[list].bin_addr == 0) && (bin_map[list].size != 0)) {
				sprintf(bin_map[list].name, "Header");
			} else {
				sprintf(bin_map[list].name, "Info-%d", (list - ilm_dlm_num));
			}
		}

		/*
		 * [3] parsing overlay section header info
		 * sram_addr : size : bin_addr : crc (16-bytes)
		 */
		if (list >= (ilm_dlm_num + info_sec_num)) {
			/* overlay info located at DLM (list = 1) start addr */
			pos = bin_map[1].bin_addr + (0x10 * (list- ilm_dlm_num - info_sec_num));

			memcpy(&bin_map[list].sram_addr, &(fwdata[pos]), 4);
			memcpy(&bin_map[list].size, &(fwdata[pos+4]), 4);
			memcpy(&bin_map[list].bin_addr, &(fwdata[pos+8]), 4);
			memcpy(&bin_map[list].crc, &(fwdata[pos+12]), 4);

			if (!hw_crc) {
				dev_err(dev, "%s %d sw_crc not support", __func__, __LINE__);
				return -EINVAL;
			}

			sprintf(bin_map[list].name, "Overlay-%d", (list- ilm_dlm_num - info_sec_num));
		}

		/* BIN size error detect */
		if ((bin_map[list].bin_addr + bin_map[list].size) > fwsize) {
			dev_err(dev, "access range (0x%08X to 0x%08X) is larger than bin size!\n",
					bin_map[list].bin_addr, bin_map[list].bin_addr + bin_map[list].size);
			return -EINVAL;
		}

		dev_dbg(dev, "[%d][%s] SRAM (0x%08X), SIZE (0x%08X), BIN (0x%08X), CRC (0x%08X)\n",
			      list, bin_map[list].name,
			      bin_map[list].sram_addr, bin_map[list].size,  bin_map[list].bin_addr, bin_map[list].crc);
	}

	return 0;
}

/* Not called: every oversized transfer poisons the GPI engine (see handoff6 §10). */
static void __maybe_unused nt36xxx_tx_size_sweep(struct nt36xxx_ts *ts);

static int nt36xxx_sky_write_part(struct nt36xxx_ts *ts, int idx);
static void nt36xxx_sky_dump_crc(struct nt36xxx_ts *ts);

/*
 * Retail order (nt36xxx_fw_update.c, nvt_download_firmware_hw_crc):
 *   nvt_bootloader_reset() -> nvt_set_bld_hw_crc() -> nvt_write_firmware() ...
 * The ILM/DLM descriptor, length and golden-CRC banks are programmed *before*
 * the firmware is written, not after it.  Writing them afterwards was our only
 * ordering deviation from the retail download.
 */
static void nt36xxx_set_bld_hw_crc(struct nt36xxx_ts *ts)
{
	int i;

	for (i = 0; i < ts->fw_data.partition; i++) {
		u32 des, len, golden;

		if (!ts->bin_map[i].size)
			continue;

		if (!strncmp(ts->bin_map[i].name, "ILM", 3)) {
			des = MMAP_ILM_DES_ADDR;
			len = MMAP_ILM_LENGTH_ADDR;
			golden = MMAP_G_ILM_CHECKSUM_ADDR;
		} else if (!strncmp(ts->bin_map[i].name, "DLM", 3)) {
			des = MMAP_DLM_DES_ADDR;
			len = MMAP_DLM_LENGTH_ADDR;
			golden = MMAP_G_DLM_CHECKSUM_ADDR;
		} else {
			continue;
		}

		regmap_raw_write(ts->regmap, ts->mmap[des],
				 &ts->bin_map[i].sram_addr, 3);
		regmap_raw_write(ts->regmap, ts->mmap[len],
				 &ts->bin_map[i].size, 3);
		regmap_raw_write(ts->regmap, ts->mmap[golden],
				 &ts->bin_map[i].crc,
				 sizeof(ts->bin_map[i].crc));
	}
}

static int32_t nt36xxx_download_firmware_hw_crc(struct nt36xxx_ts *ts) {
	uint32_t list = 0;
	uint32_t bin_addr, sram_addr, size;
	struct nvt_ts_bin_map *bin_map = ts->bin_map;

	nt36xxx_bootloader_reset(ts);

	/* retail: descriptors and golden CRCs are programmed before the write */
	nt36xxx_set_bld_hw_crc(ts);

	for (list = 0; list < ts->fw_data.partition; list++) {
		int j, ret;

		/* initialize variable */
		sram_addr = bin_map[list].sram_addr;
		size = bin_map[list].size;
		bin_addr = bin_map[list].bin_addr;

		/* ignore reserved partition (Reserved Partition size is zero) */
		if (!size) {
			dev_dbg(ts->dev, "found empty part %d. skipping ", list);
			continue;
		} else {
			size = size + 1;
			dev_dbg(ts->dev, "found useful part %d. size 0x%x ", list, size);
		}

		bin_map[list].loaded = 1;

		if (!ts->quiet_init)
			dev_info(ts->dev,
				 "sky-fw: part %u %s sram=0x%x size=0x%x chunk=0x%x\n",
				 list, bin_map[list].name, sram_addr, size,
				 NT36XXX_TRANSFER_LEN);

		for (j = 0; j < size; j += NT36XXX_TRANSFER_LEN) {
			int window_size = ((size - j) / NT36XXX_TRANSFER_LEN) ? NT36XXX_TRANSFER_LEN :
						((size - j) % NT36XXX_TRANSFER_LEN);

			if (!ts->quiet_init)
				dev_info(ts->dev,
					 "sky-fw: write off=0x%x len=%d\n", j,
					 window_size);
			ret = regmap_bulk_write(ts->regmap, sram_addr + j,
						&ts->fw_entry.data[bin_addr + j],
						window_size);
			if (ret) {
				dev_err(ts->dev, "sky-fw: write off=0x%x len=%d failed %d\n",
					j, window_size, ret);
				return ret;
			}
		}

	}

	for (list = 0; list < ts->fw_data.partition; list++) {
		if (!ts->bin_map[list].size)
			continue;
		if (nt36xxx_sky_write_part(ts, list)) {
			dev_err(ts->dev, "sky-repair: %s still damaged\n",
				ts->bin_map[list].name);
			return -EIO;
		}
	}

	return 0;
}

#define SKY_BLOCK 4096

/* return the number of 4 KiB blocks whose SRAM content differs from the image */
static int nt36xxx_sky_bad_blocks(struct nt36xxx_ts *ts, int idx, u32 *bad, int maxbad)
{
	u32 off, size;
	int nbad = 0;
	u8 *buf;

	if (!ts->bin_map || !ts->fw_entry.data || !ts->bin_map[idx].size)
		return 0;

	size = ts->bin_map[idx].size + 1;
	buf = kmalloc(SKY_BLOCK, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (off = 0; off < size; off += SKY_BLOCK) {
		u32 n = min((u32)SKY_BLOCK, size - off);

		if (regmap_raw_read(ts->regmap, ts->bin_map[idx].sram_addr + off, buf, n)) {
			dev_err(ts->dev, "sky-repair: %s read failed at 0x%x\n",
				ts->bin_map[idx].name, off);
			continue;
		}
		if (memcmp(buf, ts->fw_entry.data + ts->bin_map[idx].bin_addr + off, n)) {
			if (nbad < maxbad)
				bad[nbad] = off;
			nbad++;
		}
	}
	kfree(buf);
	return nbad;
}

/* write one partition, then read it back and rewrite damaged blocks */
static int nt36xxx_sky_write_part(struct nt36xxx_ts *ts, int idx)
{
	u32 sram = ts->bin_map[idx].sram_addr;
	u32 bin = ts->bin_map[idx].bin_addr;
	u32 size = ts->bin_map[idx].size + 1;
	u32 bad[32];
	int attempt, nbad, i, ret;

	for (attempt = 0; attempt < 4; attempt++) {
		nbad = nt36xxx_sky_bad_blocks(ts, idx, bad, ARRAY_SIZE(bad));
		if (!ts->quiet_init)
			dev_info(ts->dev,
				 "sky-repair: %s round %d bad=%d/%d\n",
				 ts->bin_map[idx].name, attempt, nbad,
				 (size + SKY_BLOCK - 1) / SKY_BLOCK);
		if (nbad <= 0)
			return 0;
		for (i = 0; i < nbad && i < (int)ARRAY_SIZE(bad); i++) {
			u32 off = bad[i];
			u32 n = min((u32)SKY_BLOCK, size - off);

			ret = regmap_bulk_write(ts->regmap, sram + off,
						&ts->fw_entry.data[bin + off], n);
			if (ret) {
				dev_err(ts->dev, "sky-repair: rewrite 0x%x failed %d\n", off, ret);
				return ret;
			}
		}
	}
	return -EIO;
}

static void _nt36xxx_boot_download_firmware(struct nt36xxx_ts *ts) {
	int i, ret, retry = 0;
	size_t fw_need_write_size = 0;
	u8 val[8 * 4] = {0};

	WARN_ON(ts->hw_crc != 2);

	/* supposed we need to load once and use many time */
	if (ts->fw_entry.data)
		goto upload;

	/* The image was loaded in probe; the worker must not call
	 * request_firmware() (see docs/handoff6 section 8). */
	if (!ts->fw_raw) {
		dev_err(ts->dev, "firmware not preloaded (%s)\n", ts->firmware_path);
		goto exit;
	}

	ts->fw_entry.data = ts->fw_raw;
	ts->fw_entry.size = ts->fw_raw_size;

	/* first firmware byte is 0x00 on this image; no sanity check here */

	for (i = (ts->fw_entry.size / 4096); i > 0; i--) {
		if (strncmp(&ts->fw_entry.data[i * 4096 - 3], "NVT", 3) == 0) {
			fw_need_write_size = i * 4096;
			break;
		}

		if (strncmp(&ts->fw_entry.data[i * 4096 - 3], "MOD", 3) == 0) {
			fw_need_write_size = i * 4096;
			break;
		}
	}

	if (fw_need_write_size == 0) {
		dev_err(ts->dev, "fw parsing error\n");
		goto release_fw;
	}

	if (*(ts->fw_entry.data + (fw_need_write_size - 4096)) + *(ts->fw_entry.data +
						((fw_need_write_size - 4096) + 1)) != 0xFF) {
		dev_err(ts->dev, "bin file FW_VER + FW_VER_BAR should be 0xFF!");
		dev_err(ts->dev, "FW_VER=0x%02X, FW_VER_BAR=0x%02X\n",
					*(ts->fw_entry.data+(fw_need_write_size - 4096)),
					*(ts->fw_entry.data+(fw_need_write_size - 4096 + 1)));
		goto release_fw;
	}

	ts->fw_data.ilm_dlm_num = 2;

	ret = nvt_bin_header_parser(ts->dev, ts->hw_crc, ts->fw_entry.data, ts->fw_entry.size,
			&ts->bin_map, &ts->fw_data.partition, ts->fw_data.ilm_dlm_num);
	if (ret) {
		if(ret != -ENOMEM)
			goto release_fw_buf;
		goto release_fw;
	}

upload:
	if (ts->hw_crc) {
		ret = nt36xxx_download_firmware_hw_crc(ts);
		if (ret) {
			dev_err(ts->dev, "nt36xxx_download_firmware_hw_crc fail!");
			goto release_fw_buf;
		}

	} else {
		dev_err(ts->dev, "non-hw_crc model is not support yet!");
		goto release_fw_buf;
	}


	/* nvt_bld_crc_enable() */
	/* crc enable */
	regmap_raw_read(ts->regmap, ts->mmap[MMAP_BLD_CRC_EN_ADDR], val, 1);

	val[0] |= 1 << 7;
	regmap_raw_write(ts->regmap, ts->mmap[MMAP_BLD_CRC_EN_ADDR], val, 1);

	/* enable fw crc */
	val[0] = 0;
	regmap_raw_write(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR] | NT36XXX_EVT_RESET_COMPLETE, val, 1);

	val[0] = 0xae;
	regmap_raw_write(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR] | NT36XXX_EVT_HOST_CMD, val, 1);

	/* nvt_boot_ready() */
	/* Set Boot Ready Bit */
	val[0] = 0x1;
	regmap_raw_write(ts->regmap, ts->mmap[MMAP_BOOT_RDY_ADDR], val, 1);

	/* old logic 5ms, retention to 10ms */
	usleep_range(10000, 11000);

	nt36xxx_sky_dump_crc(ts);

	/* nvt_check_fw_reset_state() */
	ret = nt36xxx_check_reset_state(ts, NT36XXX_STATE_INIT);
	if (ret) {
		nt36xxx_sky_dump_crc(ts);
		goto release_fw_buf;
	}


check_fw:
	/* nvt_get_fw_info() */
	ret = regmap_raw_read(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR] | NT36XXX_EVT_FWINFO, val, 16);
	if (ret)
		goto release_fw_buf;

	dev_dbg(ts->dev, "Get fw_ver=%d, max_x=%d, max_y=%d\n",
				val[0], ts->prop.max_x, ts->prop.max_y);

	/*
	 * The vendor checks fw_ver + ~fw_ver == 0xFF (buf[1]+buf[2] in its
	 * indexing, i.e. val[0]+val[1] here).  The previous port checked only
	 * val[0] != 0xff, which reports a valid FWINFO as broken.
	 */
	if ((u8)(val[0] + val[1]) != 0xff) {
		dev_err(ts->dev, "FW info is broken! fw_ver=0x%02X, ~fw_ver=0x%02X, raw=%*phN\n",
			val[0], val[1], 8, val);
		if (++retry < 5)
			goto check_fw;
		goto release_fw_buf;
	}
	dev_info(ts->dev, "sky-fwinfo: fw_ver=0x%02X ~=0x%02X raw=%*phN\n",
		 val[0], val[1], 8, val);

	/*
	 * Passing the INIT check only means the firmware was accepted, not
	 * that it is scanning: the IC runs its ReK baseline phase next and
	 * until that finishes the report area holds no point at all, while
	 * GPIO13 already toggles.  The vendor waits for RESET_STATE_REK before
	 * it unmasks the interrupt; do the same, so the first interrupt the
	 * driver serves can actually carry a report.  A late IC is not fatal
	 * here: the bounded runtime recovery covers that case.
	 */
	ret = nt36xxx_check_reset_state(ts, NT36XXX_STATE_REK);
	if (ret) {
		/* FWINFO can be readable while the IC is still stuck in INIT.
		 * Do not publish a half-initialized download as usable: doing so
		 * suppresses the next panel-cycle reinit and leaves touch dead. */
		dev_err(ts->dev, "sky: IC failed to reach ReK\n");
		goto release_fw_buf;
	}

	if (!nt36xxx_wait_first_report(ts))
		goto release_fw_buf;

	dev_info(ts->dev, "Touch IC fw loaded ok");

	ts->status |= NT36XXX_STATUS_DOWNLOAD_COMPLETE;
	goto exit;

release_fw_buf:
	kfree(ts->bin_map);
	ts->bin_map = NULL;
release_fw:
	/*
	 * fw_entry.data aliases the devm-managed fw_raw buffer (loaded in
	 * probe); freeing it here would kfree() a pointer into the middle of a
	 * devres allocation and corrupt the device's devres list.
	 */
	ts->fw_entry.data = NULL;
	ts->fw_entry.size = 0;
exit:
	return;
}

/*yell*/
/*
 * sky: GPI-DMA transmit size probe.
 *
 * The QUP GPI engine completes small MEM_TO_DEV DMA TREs (the 9-byte
 * chip-ID transfer works) but never retires the 64509-byte TRE of the
 * firmware write, and the SPI core then times out after one second.
 * Measure the boundary instead of guessing: write zeroes into the start of
 * the bootloader SRAM (always overwritten by a real download) with growing
 * lengths and stop at the first failure.
 *
 * The buffer is a plain DMA-capable kzalloc rather than a devres allocation,
 * so this measurement does not depend on the driver's devres state.
 */
static void nt36xxx_tx_size_sweep(struct nt36xxx_ts *ts)
{
	static const u32 sizes[] = { 8, 9, 16, 32, 64, 128, 256, 512, 1024,
				     2048, 4096, 8192, 16384, 32768, 64509 };
	u8 *buf;
	u32 i;

	buf = kzalloc(sizes[ARRAY_SIZE(sizes) - 1], GFP_KERNEL | GFP_DMA);
	if (!buf) {
		dev_err(ts->dev, "sky-sweep: buffer allocation failed\n");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		u32 len = sizes[i];
		int ret;

		dev_info(ts->dev, "sky-sweep: try len=%u\n", len);
		ret = regmap_bulk_write(ts->regmap, 0, buf, len);
		dev_info(ts->dev, "sky-sweep: len=%u ret=%d\n", len, ret);
		if (ret) {
			dev_info(ts->dev,
				 "sky-sweep: limit is between %u and %u bytes\n",
				 i ? sizes[i - 1] : 0u, len);
			break;
		}
	}

	kfree(buf);
}

/**
 * nt36xxx_sky_schedule_recover - Re-run the IC init after a firmware reset
 * @ts: Main driver structure
 * @why: Short reason, used for the log line
 *
 * A firmware that fell back to its reset state never presents a report again,
 * so reading the report area alone cannot recover it: the IC has to be
 * re-initialised.  Recovery is rate limited and bounded, otherwise a
 * permanently dead IC would turn into the re-download livelock that the
 * all-0xFE idle-pattern guard was introduced to stop.
 */
static void nt36xxx_sky_schedule_recover(struct nt36xxx_ts *ts, const char *why)
{
	unsigned long spacing;

	if (ts->status & NT36XXX_STATUS_SUSPEND)
		return;

	/*
	 * Five attempts two seconds apart, then one per minute for as long as
	 * the IC stays dead: a wedged controller must not keep the bus busy,
	 * but it also must not be abandoned, because whatever killed the
	 * firmware (ESD, a disturbed bus) can go away on its own.
	 */
	if (ts->recover_tries < NT36XXX_RECOVER_LIMIT)
		spacing = NT36XXX_RECOVER_FAST_MS;
	else
		spacing = NT36XXX_RECOVER_SLOW_MS;

	if (ts->recover_time &&
	    time_before(jiffies, ts->recover_time + msecs_to_jiffies(spacing)))
		return;

	/*
	 * After the fast phase and a few slow attempts the IC is not coming
	 * back by itself (measured: five failed re-inits end in a 200 IRQ/s
	 * storm with every report read returning 0xFE).  Stop touching it and
	 * leave the IC alone until a panel prepare or a reboot; the interrupt
	 * storm then costs one early return instead of an SPI read each.
	 */
	if (ts->recover_tries >= NT36XXX_RECOVER_LIMIT + NT36XXX_RECOVER_SLOW_TRIES) {
		if (!ts->give_up) {
			ts->give_up = true;
			dev_err(ts->dev,
				"sky: IC will not recover (%u attempts), parking until panel cycle/reboot\n",
				ts->recover_tries);
		}
		return;
	}

	ts->recover_time = jiffies;
	ts->recover_tries++;
	ts->fw_reset_cnt = 0;
	ts->quiet_init = ts->recover_tries > NT36XXX_RECOVER_LIMIT;

	dev_info(ts->dev, "sky-recover[%u]: %s, re-running IC init\n",
		 ts->recover_tries, why);

	ts->status &= ~NT36XXX_STATUS_DOWNLOAD_COMPLETE;
	sky_fw_attempts = 0;
	schedule_delayed_work(&ts->work, 0);
}

/**
 * nt36xxx_health_work - Periodic firmware state check
 * @work: Health work
 *
 * The interrupt line only fires while the IC still drives it, so a firmware
 * that went quiet would never be noticed by the report path.  The vendor
 * polls its ESD watchdog the same way.  Only the explicit firmware-reset
 * markers are acted on, so an IC whose register reads anything else is never
 * re-initialised on a guess.
 */
static void nt36xxx_health_work(struct work_struct *work)
{
	struct nt36xxx_ts *ts = container_of(work, struct nt36xxx_ts,
					     health_work.work);
	u8 state[6] = { 0 };
	int ret;

	/* suspended: internal_pm_resume() schedules this again */
	if (ts->status & NT36XXX_STATUS_SUSPEND)
		return;

	if ((ts->status & NT36XXX_STATUS_DOWNLOAD_COMPLETE) && ts->mmap) {
		mutex_lock(&ts->lock);
		ret = regmap_raw_read(ts->regmap,
				      ts->mmap[MMAP_EVENT_BUF_ADDR] |
						      NT36XXX_EVT_RESET_COMPLETE,
				      state, 6);
		mutex_unlock(&ts->lock);

		/*
		 * 0xFD/0xFE/0xFF: the firmware reset.  Anything below REK
		 * (0xA1) means the firmware is stuck before it ever scans - the
		 * state a display off/on cycle leaves behind, and one that never
		 * produces an interrupt, so this poll is the only detector.
		 */
		if (!ret && (state[0] == 0xFD || state[0] == 0xFE ||
			     state[0] == 0xFF ||
			     state[0] < NT36XXX_STATE_REK)) {
			if (++ts->health_bad >= 2)
				nt36xxx_sky_schedule_recover(ts,
							     "fw state reset");
		} else if (!ret) {
			ts->health_bad = 0;

			/* a minute of healthy state earns a fresh fast phase */
			if (ts->recover_tries && ts->recover_time &&
			    time_after(jiffies,
				       ts->recover_time +
					       msecs_to_jiffies(NT36XXX_RECOVER_SLOW_MS))) {
				ts->recover_tries = 0;
				ts->quiet_init = false;
			}
		}
	}

	schedule_delayed_work(&ts->health_work,
			      msecs_to_jiffies(NT36XXX_HEALTH_PERIOD_MS));
}

static void nt36xxx_download_firmware(struct work_struct *work) {
	struct nt36xxx_ts *ts = container_of(work, struct nt36xxx_ts, work.work);
	int ret;

	/* Disable power management runtime for the device */
	pm_runtime_disable(ts->dev);

	/* Disable the touch screen IRQ to prevent further interrupts */
	disable_irq_nosync(ts->irq);

	/* Cancel any pending delayed work */
	cancel_delayed_work(&ts->work);

	mutex_lock(&ts->lock);

	/*
	 * Always use the retail reset ladder (0xAA / 0x5A / 0x55) before the
	 * download.  With only the engineering reset the first init after
	 * boot reliably ends in the 0xFE reset state, and the very same
	 * ladder on the retry fixes it - so it belongs on every attempt, not
	 * just the retries.
	 */
	if (ts->recover_tries || sky_fw_attempts)
		nt36xxx_sky_dump_fw_history(ts);

	nt36xxx_sky_recover_reset(ts);

	/* Set memory maps for the specific chip version */
	ret = nt36xxx_chip_version_init(ts);
	if (ret) {
		dev_err(ts->dev, "Failed to check chip version\n");
		mutex_unlock(&ts->lock);
		goto skip;
	}

	/* Download firmware using the internal function */
	_nt36xxx_boot_download_firmware(ts);

	mutex_unlock(&ts->lock);

	/* Fresh init: restart the bounded per-init diagnostics and counters */
	if (ts->status & NT36XXX_STATUS_DOWNLOAD_COMPLETE) {
		ts->report_log_cnt = 0;
		ts->fw_reset_cnt = 0;
		ts->health_bad = 0;
		ts->init_once = true;
	}

skip:
	/* Enable touch screen IRQ and power management runtime */
	enable_irq(ts->irq);
	pm_runtime_enable(ts->dev);

	/* If the download is not complete, reschedule the delayed work after 4000ms */
	if (!(ts->status & NT36XXX_STATUS_DOWNLOAD_COMPLETE)) {
		if (++sky_fw_attempts < 2) {
			cancel_delayed_work(&ts->work);
			schedule_delayed_work(&ts->work, 4000);
		} else {
			dev_err(ts->dev, "sky: giving up after %d attempts\n",
				sky_fw_attempts);
		}
	}
}

static void nt36xxx_disable_regulators(void *data)
{
	struct nt36xxx_ts *ts = data;

	regulator_bulk_disable(NT36XXX_NUM_SUPPLIES, ts->supplies);
}


/**
 * sky_reinit_store - Force the panel-resume re-init from userspace
 * @dev: SPI device
 * @attr: Attribute description
 * @buf: "1" to trigger
 * @count: Size of @buf
 *
 * The retail recovery is driven by the panel: every screen-on re-flashes the
 * firmware.  Boards (and userspace knobs) do not always produce a real DPMS
 * cycle, so expose the same path directly for bring-up testing.
 *
 * Return: @count on success, negative number for error
 */
static ssize_t sky_reinit_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct nt36xxx_ts *ts = dev_get_drvdata(dev);
	unsigned int val;

	if (kstrtouint(buf, 0, &val) || val != 1)
		return -EINVAL;

	dev_info(dev, "sky-reinit: re-running the IC init\n");
	ts->give_up = false;
	ts->recover_tries = 0;
	ts->status &= ~NT36XXX_STATUS_DOWNLOAD_COMPLETE;
	sky_fw_attempts = 0;
	schedule_delayed_work(&ts->work, 0);

	return count;
}

static DEVICE_ATTR_WO(sky_reinit);

static int nt36xxx_input_dev_config(struct nt36xxx_ts *ts, const struct input_id *id)
{
	struct device *dev = ts->dev;
	int ret;

	/* Allocate memory for the input device structure */
	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	/* Set the device-specific data to the allocated input device structure */
	input_set_drvdata(ts->input, ts);

	/* Set physical path for the input device */
	ts->input->phys = devm_kasprintf(dev, GFP_KERNEL,
				     "%s/input0", dev_name(dev));
	if (!ts->input->phys)
		return -ENOMEM;

	/* Set input device properties */
	ts->input->name = "Novatek NT36XXX Touchscreen";
	ts->input->dev.parent = dev;
	ts->input->id = *id;

	/* Set absolute parameters for touch events */
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0,
						 TOUCH_MAX_PRESSURE, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0,
						 NT36XXX_TOUCH_MAJOR_MAX, 0, 0);

	/* Set absolute parameters for touch position */
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0,
						 ts->data->abs_x_max - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0,
						 ts->data->abs_y_max - 1, 0, 0);

	/* Parse touchscreen properties */
	touchscreen_parse_properties(ts->input, true, &ts->prop);

	/* Check if the maximum x-coordinate is valid */
	WARN_ON(ts->prop.max_x < 1);

	/* Initialize multitouch slots for the input device */
	ret = input_mt_init_slots(ts->input, TOUCH_MAX_FINGER_NUM,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret) {
		dev_err(dev, "Cannot init MT slots (%d)\n", ret);
		return ret;
	}

	/* Register the input device */
	ret = input_register_device(ts->input);
	if (ret) {
		dev_err(dev, "Failed to register input device: %d\n",
			ret);
		return ret;
	}

	return 0;
}

int nt36xxx_probe(struct device *dev, int irq, const struct input_id *id,
			struct regmap *regmap)
{
	const struct nt36xxx_chip_data *chip_data;
	int ret;

	/* Allocate memory for the touchscreen data structure */
	struct nt36xxx_ts *ts = devm_kzalloc(dev, sizeof(struct nt36xxx_ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	/* Set the device-specific data to the allocated structure */
	dev_set_drvdata(dev, ts);

	/* Retrieve chip-specific data from the device tree */
	chip_data = of_device_get_match_data(dev);
	if(!chip_data)
		return -EINVAL;

	/* Initialize the touchscreen structure with relevant data */
	ts->dev = dev;
	ts->regmap = regmap;
	ts->irq = irq;

	ts->data = chip_data;
	memcpy(ts->mmap_data, chip_data->mmap, sizeof(ts->mmap_data));
	ts->mmap = ts->mmap_data;

	/* Allocate memory for GPIO supplies */
	ts->supplies = devm_kcalloc(dev, NT36XXX_NUM_SUPPLIES,
				    sizeof(*ts->supplies), GFP_KERNEL);
	if (!ts->supplies)
		return -ENOMEM;

	/* This board uses the controller's SPI soft-reset command.  Do not
	 * claim GPIO12: the vendor driver leaves it unrequested. */
	ts->reset_gpio = NULL;

	/* Get and configure the optional IRQ GPIO */
	ts->irq_gpio = devm_gpiod_get_optional(dev, "irq", GPIOD_IN);
	if (IS_ERR(ts->irq_gpio))
		return PTR_ERR(ts->irq_gpio);

	/* Prefer the touch IRQ GPIO over a parent controller IRQ. */
	if (ts->irq_gpio)
		ts->irq = gpiod_to_irq(ts->irq_gpio);
	else
		ts->irq = irq;

	if (ts->irq <= 0) {
		dev_warn(dev, "no touch IRQ available, running without interrupts\n");
		ts->irq = 0;
	}
	dev_dbg(ts->dev, "Touch IRQ: %#x\n", ts->irq);

	gpiod_set_consumer_name(ts->irq_gpio, "nt36xxx irq");

	/* If the device follows a DRM panel, skip regulator initialization */
	if (drm_is_panel_follower(dev))
		goto skip_regulators;

	/* These supplies are optional, also shared with LCD panel */
	ts->supplies[0].supply = "vdd";
	ts->supplies[1].supply = "vio";
	ret = devm_regulator_bulk_get(dev,
				      NT36XXX_NUM_SUPPLIES,
				      ts->supplies);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Cannot get supplies: %d\n", ret);

	ret = regulator_bulk_enable(NT36XXX_NUM_SUPPLIES, ts->supplies);
	if (ret)
		return ret;

	/* Delay for regulators to stabilize */
	usleep_range(10000, 11000);

	ret = devm_add_action_or_reset(dev, nt36xxx_disable_regulators, ts);
	if (ret)
		return ret;

skip_regulators:
	/* Initialize mutex for synchronization */
	mutex_init(&ts->lock);

	/* Check and configure the touch screen chip */
	ret = nt36xxx_eng_reset_idle(ts);
	if (ret) {
		dev_err(dev, "Failed to check chip version\n");
		return ret;
	}

	/* Set memory maps for the specific chip version */
	ret = nt36xxx_chip_version_init(ts);
	if (ret) {
		dev_err(dev, "Failed to check chip version\n");
		return ret;
	}

	/* Parse the firmware path from the device tree */
	ret = of_property_read_string(dev->of_node, "firmware-name", &ts->firmware_path);
	if (ret) {
		dev_err(dev, "Failed to read firmware-name property\n");
		return ret;
	}

	/*
	 * sky: load the firmware here, in process context, and keep a
	 * DMA-capable copy.  Requesting it later from the download worker
	 * crashed inside devres_find() (ts->dev->devres_head.prev == -1) and
	 * killed the worker with IRQs disabled; see docs/handoff6 section 8.
	 */
	{
		const struct firmware *fw = NULL;

		ret = request_firmware(&fw, ts->firmware_path, dev);
		if (ret) {
			dev_err(dev, "Failed to load %s: %d\n", ts->firmware_path, ret);
		} else {
			ts->fw_raw = devm_kmemdup(dev, fw->data, fw->size,
						  GFP_KERNEL | GFP_DMA);
			if (ts->fw_raw) {
				ts->fw_raw_size = fw->size;
				dev_info(dev, "firmware %s loaded, %zu bytes\n",
					 ts->firmware_path, ts->fw_raw_size);
			}
			release_firmware(fw);
			if (!ts->fw_raw)
				ret = -ENOMEM;
		}
	}
	if (ret)
		return ret;

	/* Сopy the const mmap into drvdata */
	memcpy(ts->mmap_data, ts->data->mmap, sizeof(ts->mmap_data));
	ts->mmap = ts->mmap_data;

	ret = nt36xxx_input_dev_config(ts, ts->data->id);
	if (ret) {
			dev_err(dev, "failed set input device: %d\n", ret);
			return ret;
	}

	/* Request threaded IRQ for touch screen interrupts */
	if (ts->irq > 0) {
		ret = devm_request_threaded_irq(dev, ts->irq, NULL, nt36xxx_irq_handler,
				 IRQ_TYPE_EDGE_RISING | IRQF_ONESHOT, dev_name(dev), ts);
		if (ret) {
			dev_err(dev, "request irq failed: %d\n", ret);
			return ret;
		}

		/* what the irqchip ended up with, after the DT active-low flag */
		dev_info(dev, "sky-irq: irq=%d trigger=0x%x\n", ts->irq,
			 irq_get_trigger_type(ts->irq));
	}

	/* bring-up knob: replay the panel-resume re-init on demand */
	ret = device_create_file(dev, &dev_attr_sky_reinit);
	if (ret)
		dev_warn(dev, "cannot create sky_reinit: %d\n", ret);

	/* Set up delayed work for firmware download */
	devm_delayed_work_autocancel(dev, &ts->work, nt36xxx_download_firmware);
	devm_delayed_work_autocancel(dev, &ts->health_work, nt36xxx_health_work);

	schedule_delayed_work(&ts->health_work,
			      msecs_to_jiffies(NT36XXX_HEALTH_PERIOD_MS));

	/*
	 * The retail driver defers its first firmware download until "after the
	 * display reset (RESX) sequence", i.e. 14 s, because touching the IC
	 * before the panel is up leaves it in its reset state.  We have the
	 * panel callback, so the follower runs the init as soon as the panel is
	 * prepared; the timer below is only the fallback for a board whose
	 * panel never reports prepare.
	 */
	if (drm_is_panel_follower(dev)) {
		ts->panel_follower.funcs = &nt36xxx_panel_follower_funcs;
		devm_drm_panel_add_follower(dev, &ts->panel_follower);
	}

	/*
	 * Retail defers its very first download to "after the display reset
	 * (RESX) sequence": a 14 s delayed work with no callback involvement.
	 * Our first attempt used to run at panel_prepared (~2.6 s), i.e. while
	 * the DSI link and the first frames were still settling - measured as a
	 * failure in every single boot, followed by recovery attempts that only
	 * sometimes succeeded.  Use the retail value verbatim.
	 */
	schedule_delayed_work(&ts->work, msecs_to_jiffies(NT36XXX_FIRST_INIT_MS));

	dev_info(dev, "Novatek touchscreen initialized\n");
	return 0;
}

EXPORT_SYMBOL_GPL(nt36xxx_probe);

static int __maybe_unused nt36xxx_internal_pm_suspend(struct device *dev)
{
	struct nt36xxx_ts *ts = dev_get_drvdata(dev);
	int ret = 0;

	ts->status |= NT36XXX_STATUS_SUSPEND;

	/* cancel before taking the lock: the worker holds it while running */
	cancel_delayed_work_sync(&ts->work);
	cancel_delayed_work_sync(&ts->health_work);

	/*
	 * The vendor sends the sleep command to the event buffer's HOST_CMD
	 * byte (EVENT_BUF_ADDR | 0x50).  Writing it to EVENT_BUF_ADDR itself
	 * hits the report area instead of the command channel.
	 */
	mutex_lock(&ts->lock);
	if (ts->mmap[MMAP_EVENT_BUF_ADDR]) {
		ret = regmap_write(ts->regmap,
				   ts->mmap[MMAP_EVENT_BUF_ADDR] |
					   NT36XXX_EVT_HOST_CMD,
				   NT36XXX_CMD_ENTER_SLEEP);
	}
	mutex_unlock(&ts->lock);

	if (ret)
		dev_err(ts->dev, "Cannot enter suspend!\n");
	return 0;
}

static int __maybe_unused nt36xxx_pm_suspend(struct device *dev)
{
	struct nt36xxx_ts *ts = dev_get_drvdata(dev);
	int ret=0;

	if (drm_is_panel_follower(dev))
		return 0;

	disable_irq_nosync(ts->irq);

	ret = nt36xxx_internal_pm_suspend(dev);
	return ret;
}

static int __maybe_unused nt36xxx_internal_pm_resume(struct device *dev)
{
	struct nt36xxx_ts *ts = dev_get_drvdata(dev);

	/*
	 * Bookkeeping only.  The re-flash belongs to the "display turned on"
	 * event (panel_enabled / pm_resume), because m19_prepare() only raises
	 * the rails while the DSI init runs in m19_enable(); scheduling it any
	 * earlier lands the download before the panel is ready.
	 */
	ts->status &= ~NT36XXX_STATUS_SUSPEND;
	ts->quiet_init = false;

	schedule_delayed_work(&ts->health_work,
			      msecs_to_jiffies(NT36XXX_HEALTH_PERIOD_MS));

	return 0;
}

static int __maybe_unused nt36xxx_pm_resume(struct device *dev)
{
	struct nt36xxx_ts *ts = dev_get_drvdata(dev);
	int ret=0;

	if (drm_is_panel_follower(dev))
		return 0;

	enable_irq(ts->irq);

	if (ts->init_once)
		mod_delayed_work(system_wq, &ts->work, msecs_to_jiffies(40));

	ret = nt36xxx_internal_pm_resume(dev);
	return ret;
}

EXPORT_GPL_SIMPLE_DEV_PM_OPS(nt36xxx_pm_ops,
			     nt36xxx_pm_suspend,
			     nt36xxx_pm_resume);

static int panel_prepared(struct drm_panel_follower *follower)
{
	struct nt36xxx_ts *ts = container_of(follower, struct nt36xxx_ts, panel_follower);

	dev_info(ts->dev, "sky-panel: prepared\n");

	if (ts->status & NT36XXX_STATUS_SUSPEND)
		enable_irq(ts->irq);

	/* supposed to clear the flag, but leave to internal_pm_resume
	for greater purpose */
	/* ts->status &= ~NT36XXX_STATUS_SUSPEND; */

	return nt36xxx_internal_pm_resume(ts->dev);
}

static int panel_unpreparing(struct drm_panel_follower *follower)
{
	struct nt36xxx_ts *ts = container_of(follower, struct nt36xxx_ts, panel_follower);

	dev_info(ts->dev, "sky-panel: unpreparing\n");

	ts->status |= NT36XXX_STATUS_SUSPEND;

	disable_irq(ts->irq);

	return nt36xxx_internal_pm_suspend(ts->dev);
}

/*
 * The retail driver hangs its whole power state machine on the panel blank
 * notifier, not on the rail events:
 *
 *   XIAOMI_PANEL_EARLY_EVENT_BLANK + blank=1 -> nvt_ts_suspend()  (IC sleeps)
 *   XIAOMI_PANEL_NORMAL_EVENT_BLANK + blank=0 -> resume work, 40 ms later,
 *                                                and it re-flashes the firmware
 *
 * The DRM equivalent of "display turned on" is drm_panel_enable(), i.e. the
 * follower's panel_enabled: m19_prepare() only raises the rails, all DSI
 * initialisation happens in m19_enable().  Implementing only prepared/
 * unpreparing meant a screen-on cycle never reached this driver at all.
 */
static int panel_enabled(struct drm_panel_follower *follower)
{
	struct nt36xxx_ts *ts = container_of(follower, struct nt36xxx_ts, panel_follower);

	dev_info(ts->dev, "sky-panel: enabled\n");

	ts->give_up = false;
	ts->recover_tries = 0;

	/*
	 * Retail re-flashes the firmware on every resume, 40 ms after the
	 * unblank event.  The first bring-up is the exception: it stays on the
	 * retail 14 s timer.
	 */
	if (ts->init_once)
		mod_delayed_work(system_wq, &ts->work, msecs_to_jiffies(40));

	return nt36xxx_internal_pm_resume(ts->dev);
}

static int panel_disabling(struct drm_panel_follower *follower)
{
	struct nt36xxx_ts *ts = container_of(follower, struct nt36xxx_ts, panel_follower);

	dev_info(ts->dev, "sky-panel: disabling\n");

	ts->status |= NT36XXX_STATUS_SUSPEND;

	return nt36xxx_internal_pm_suspend(ts->dev);
}

static struct drm_panel_follower_funcs nt36xxx_panel_follower_funcs = {
	.panel_prepared = panel_prepared,
	.panel_enabled = panel_enabled,
	.panel_disabling = panel_disabling,
	.panel_unpreparing = panel_unpreparing,
};

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NT36XXX Touchscreen driver");
MODULE_AUTHOR("AngeloGioacchino Del Regno <kholk11@gmail.com>");
MODULE_AUTHOR("99degree <www.github.com/99degree>");
