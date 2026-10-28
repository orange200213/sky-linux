/*
 * sky M1 boot shim -- Xiaomi Redmi 12 5G (SM4450 "ravelin", product "river").
 *
 * Structure and payload format follow sunstone-linux's shim for SM4375
 * (start.S / loader.c / console.c are reused; this file replaces shim.c with
 * the sky-specific, M1-scoped main path).
 *
 * M1 scope: print what ABL/UEFI handed over, find the SWEETLNX payload behind
 * _end, copy Image+DTB and jump. No GIC/PSCI/MMU tests, no watchdog, no PMIC
 * writes: nothing is changed except the framebuffer we draw on.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"

extern char _end[];

/* qcom,msm-imem@146aa000 + restart_reason@65c (kept for later milestones) */
#define IMEM_RESTART_REASON	0x146aa65cUL

static inline uint64_t rd_cntpct(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static inline uint32_t rd_cntfrq(void)
{
	uint32_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static inline uint64_t rd_currentel(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, currentel" : "=r"(v));
	return v;
}

static inline uint64_t rd_sctlr_el1(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
	return v;
}

static inline uint64_t rd_esr_el1(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, esr_el1" : "=r"(v));
	return v;
}

static inline uint64_t rd_elr_el1(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, elr_el1" : "=r"(v));
	return v;
}

static inline uint64_t rd_far_el1(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, far_el1" : "=r"(v));
	return v;
}

static void delay_ms(unsigned ms)
{
	uint32_t frq = rd_cntfrq();

	if (!frq)
		frq = 19200000;		/* XO, if the firmware left CNTFRQ at 0 */
	uint64_t ticks = (uint64_t)ms * frq / 1000;
	uint64_t t0 = rd_cntpct();

	while (rd_cntpct() - t0 < ticks)
		__asm__ volatile("yield");
}

/* console.c calls this for its page-break helper */
void con_hold_ms(unsigned ms)
{
	delay_ms(ms);
}

/* A 2D char array (not a table of pointers): the shim runs at an arbitrary
 * load PA, so no link-time absolute addresses may appear in .rodata. */
static const char exc_names[16][16] = {
	"cur-sp0-sync", "cur-sp0-irq", "cur-sp0-fiq", "cur-sp0-serror",
	"cur-spx-sync", "cur-spx-irq", "cur-spx-fiq", "cur-spx-serror",
	"low-a64-sync", "low-a64-irq", "low-a64-fiq", "low-a64-serror",
	"low-a32-sync", "low-a32-irq", "low-a32-fiq", "low-a32-serror",
};

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_clear(RED);
	con_str("EXCEPTION", exc_names[slot & 15]);
	con_hex("ESR_EL1", rd_esr_el1());
	con_hex("ELR_EL1", rd_elr_el1());
	con_hex("FAR_EL1", rd_far_el1());
	con_hex("SP", (uint64_t)regs);
	con_str("HALT", "exception in shim");
	for (;;)
		__asm__ volatile("wfe");
}

static int magic_ok(const char *m)
{
	static const char want[8] = { 'S', 'W', 'E', 'E', 'T', 'L', 'N', 'X' };

	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

/* PSCI CPU_ON target referenced by start.S; M1 never starts secondaries, the
 * kernel will do that itself through PSCI. Park any stray core. */
void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main(uint64_t dtb)
{
	/* only paint the top band: if the framebuffer address is wrong we touch
	 * as little reserved memory as possible, and the ABL splash stays visible */
	con_begin(BLUE, 14);
	con_str("SHIM", "sky M1 (SM4450 ravelin)");
	con_hex2("CPU", "el=", (uint32_t)(rd_currentel() >> 2), "cntfrq=", rd_cntfrq());
	con_hex2("STATE", "sctlr_m=", (uint32_t)(rd_sctlr_el1() & 1), "abl_dtb=", (uint32_t)dtb);
	con_hex2("FB", "base=", (uint32_t)FB_BASE, "stride=", FB_STRIDE);
	con_hex("_end", (uint64_t)_end);
	con_skip();

	uintptr_t emb = ((uintptr_t)_end + 0xfff) & ~0xfffUL;

	con_hex("PAYLOAD", emb);
	if (!magic_ok((const char *)emb)) {
		con_str("RESULT", "no SWEETLNX payload behind _end");
		con_str("HALT", "flash a boot image built by build-sky.sh");
		for (;;)
			__asm__ volatile("wfe");
	}
	con_str("RESULT", "SWEETLNX found");

	/* loader.c prints IMAGE/LOAD/DTB lines and does the copy + jump */
	const char *why = linux_boot(emb);

	con_str("LOADER", why);
	con_skip();
	con_str("HALT", "payload did not start Linux");
	for (;;)
		__asm__ volatile("wfe");
}
