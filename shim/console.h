#pragma once
#include <stdint.h>

/*
 * sky: ABL leaves its continuous splash at the start of reserved-memory
 * splash_region@0xb8000000 (see docs/C-sky-hw-params.md). Geometry from the
 * panel node: 1080x2460, 4 bytes per pixel, stride = width * 4.
 * Override with -DSHIM_FB_BASE/-DSHIM_FB_W/-DSHIM_FB_H if the address turns
 * out to be different once the first boot is observed.
 */
#ifndef SHIM_FB_BASE
#define SHIM_FB_BASE	0xb8000000UL
#endif
#ifndef SHIM_FB_W
#define SHIM_FB_W	1080
#endif
#ifndef SHIM_FB_H
#define SHIM_FB_H	2460
#endif

#define FB_BASE		SHIM_FB_BASE
#define FB_W		SHIM_FB_W
#define FB_H		SHIM_FB_H
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches simple-framebuffer */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define DKGREEN	0xff005000U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
/* paint only the first `rows` text rows (cheaper, keeps the ABL splash below) */
void con_begin(uint32_t bg, int rows);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
/* page break: if fewer than n rows are left, hold the page hold_ms, then clear */
void con_need(int n, uint32_t bg, unsigned hold_ms);
