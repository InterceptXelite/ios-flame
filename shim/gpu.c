/*
 * flame stage 8a: Adreno 613 power-up diagnostics (shim only, iOS not booted).
 *
 * SM4450 GPU: kgsl-3d0 @ 0x3d00000 (chipid 0x06010300, "gmu_wrapper": no GMU
 * firmware, the OS switches the power domains itself), GPU_CC @ 0x3d90000.
 * Both GDSCs hang off the CX rail (cx.lvl, already up), so no PMIC work.
 * Sequence after Linux gpucc-sm4450.c / gdsc.c / a6xx_pm_resume:
 *   GCC gpu branches -> GPU_CC ahb/cxo/hub -> CX GDSC on -> GX BCR pulse,
 *   GX GDSC clamp release + on -> gfx3d RCG = XO (19.2 MHz, no PLL) -> branch on
 *   -> CP_SCRATCH_REG0 write/readback, RBBM_STATUS.
 * Every register is read through a fault-tolerant probe; GPU core registers are
 * touched only if both GDSCs report power and the core clock is running.
 */
#include <stdint.h>
#include "console.h"
#include "gpu.h"

extern int probe_wr32(uintptr_t a, uint32_t v);
extern int probe_rd32(uintptr_t a, uint32_t *v);

static uint64_t ticks(void) { uint64_t t; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t)); return t; }
static void udelay(unsigned us) { uint64_t e = ticks() + (uint64_t)us * 192 / 10; while (ticks() < e) ; }

#define GCC		0x00100000UL
#define GPUCC		0x03d90000UL
#define GPU		0x03d00000UL
#define GREG(r)		(GPU + 4UL * (r))

static int bad;

static uint32_t R(uintptr_t a)
{
	uint32_t v = 0;
	if (!probe_rd32(a, &v)) {
		bad++;
		return 0xbad0bad0;
	}
	return v;
}

static void W(uintptr_t a, uint32_t v)
{
	if (!probe_wr32(a, v))
		bad++;
}

static void setbits(uintptr_t a, uint32_t m) { W(a, R(a) | m); }
static void clrbits(uintptr_t a, uint32_t m) { W(a, R(a) & ~m); }

/* branch: enable bit0, wait CLK_OFF (bit31) clear */
static int branch_on(uintptr_t cbcr)
{
	setbits(cbcr, 1);
	for (int i = 0; i < 500; i++) {
		if (!(R(cbcr) >> 31))
			return 1;
		udelay(2);
	}
	return 0;
}

static int poll(uintptr_t a, uint32_t mask, uint32_t want, unsigned us)
{
	for (unsigned i = 0; i < us; i++) {
		if ((R(a) & mask) == want)
			return 1;
		udelay(1);
	}
	return 0;
}

static void state(const char *tag)
{
	con_hex2(tag, "cxgdsc=", R(GPUCC + 0x9108), "gxgdsc=", R(GPUCC + 0x905c));
	con_hex2("  gx cfg", "gxcfg=", R(GPUCC + 0x9060), "clamp=", R(GPUCC + 0x9504));
	con_hex2("  cbcr", "ahb=", R(GPUCC + 0x911c), "cxo=", R(GPUCC + 0x9144));
	con_hex2("  cbcr", "gfx3d=", R(GPUCC + 0x90a8), "hubcx=", R(GPUCC + 0x9148));
	con_hex2("  rcg gfx3d", "cmd=", R(GPUCC + 0x9070), "cfg=", R(GPUCC + 0x9074));
	con_hex2("  gcc gpu", "memnoc=", R(GCC + 0x81010), "dvm=", R(GCC + 0x81018));
}

void test_gpu(void)
{
	con_str("SHIM", "ios-flame stage 8a: Adreno 613 power-up");
	con_skip();
	state("BEFORE");
	con_hex2("  pll0", "mode=", R(GPUCC + 0x0), "pll1=", R(GPUCC + 0x1000));
	con_hex("  probe faults", bad);
	if (bad) {
		con_str("GPU", "GPU_CC not readable from EL1 - stop");
		return;
	}
	con_skip();

	/* GCC side: GPLL0 to the GPU CC, GPU memnoc + SNOC DVM branches */
	setbits(GCC + 0x62000, (1u << 15) | (1u << 16));
	int ok_noc = branch_on(GCC + 0x81010) & branch_on(GCC + 0x81018);
	/* GPU_CC CX side */
	int ok_cx = branch_on(GPUCC + 0x911c) & branch_on(GPUCC + 0x9144) & branch_on(GPUCC + 0x9148);
	con_hex2("CLOCKS", "noc=", ok_noc, "cx=", ok_cx);

	/* CX GDSC (votable): clear SW_COLLAPSE, wait PWR_ON */
	clrbits(GPUCC + 0x9108, 1);
	int cx_on = poll(GPUCC + 0x9108, 1u << 31, 1u << 31, 1500);
	con_hex2("CX GDSC", "on=", cx_on, "reg=", R(GPUCC + 0x9108));
	if (!cx_on || bad) {
		con_str("GPU", "CX GDSC did not power up - stop");
		return;
	}

	/* GX GDSC: SW reset (GX, ACD, GX_ACD_IROOT BCRs), AON reset, clamp release, on */
	static const uint32_t bcr[3] = {0x9058, 0x9358, 0x958c};
	for (int i = 0; i < 3; i++)
		W(GPUCC + bcr[i], 1);
	udelay(1);
	for (int i = 0; i < 3; i++)
		W(GPUCC + bcr[i], 0);
	setbits(GPUCC + 0x9504, 1u << 4);
	udelay(1);
	clrbits(GPUCC + 0x9504, 1u << 4);
	clrbits(GPUCC + 0x9504, 1u << 0);		/* clamp off */
	clrbits(GPUCC + 0x905c, 1);			/* SW_COLLAPSE off */
	int gx_on = poll(GPUCC + 0x9060, 1u << 16, 1u << 16, 1500);	/* CFG_GDSCR POWER_UP_COMPLETE */
	udelay(1);
	setbits(GPUCC + 0x905c, 1u << 11);		/* retain FF */
	con_hex2("GX GDSC", "on=", gx_on, "reg=", R(GPUCC + 0x905c));
	con_hex2("GX GDSC", "cfg=", R(GPUCC + 0x9060), "faults=", bad);
	if (!gx_on || bad) {
		con_str("GPU", "GX GDSC did not power up - stop");
		state("NOW");
		return;
	}

	/* gfx3d RCG: XO source, no divider (CFG = 0), then UPDATE */
	W(GPUCC + 0x9074, 0);
	setbits(GPUCC + 0x9070, 1);
	poll(GPUCC + 0x9070, 1, 0, 500);
	int core = branch_on(GPUCC + 0x90a8) & branch_on(GPUCC + 0x90b8);	/* gx_gfx3d, gx_cxo */
	branch_on(GPUCC + 0x919c);					/* cx_gfx3d */
	branch_on(GPUCC + 0x91a0);					/* cx_gfx3d_slv */
	state("POWERED");
	con_hex2("CORE CLK", "on=", core, "faults=", bad);
	if (!core || bad) {
		con_str("GPU", "gfx3d clock not running - GPU registers NOT touched");
		return;
	}
	con_skip();

	/* first contact with the GPU itself */
	uint32_t st = R(GREG(0x210));
	con_hex2("RBBM", "status=", st, "faults=", bad);
	if (bad) {
		con_str("GPU", "GPU register read faulted - stop");
		return;
	}
	W(GREG(0x883), 0x12345678);
	W(GREG(0x884), 0x9abcdef0);
	uint32_t s0 = R(GREG(0x883)), s1 = R(GREG(0x884));
	con_hex2("CP SCRATCH", "0=", s0, "1=", s1);
	con_hex2("CP", "sqe_cntl=", R(GREG(0x808)), "secvid=", R(GREG(0xf400)));
	con_hex2("RBBM", "int0=", R(GREG(0x201)), "faults=", bad);
	con_str("GPU", s0 == 0x12345678 && s1 == 0x9abcdef0 ? "ADRENO 613 ALIVE (registers respond)" :
		"GPU registers do not hold values");
}
