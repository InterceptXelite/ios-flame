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

static int gpu_power(void)
{
	state("BEFORE");
	con_hex("  probe faults", bad);
	if (bad) {
		con_str("GPU", "GPU_CC not readable from EL1 - stop");
		return -1;
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
		return -1;
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
		return -1;
	}

	/* gfx3d RCG: XO source, no divider (CFG = 0), then UPDATE */
	W(GPUCC + 0x9074, 0);
	setbits(GPUCC + 0x9070, 1);
	poll(GPUCC + 0x9070, 1, 0, 500);
	int core = branch_on(GPUCC + 0x90a8) & branch_on(GPUCC + 0x90b8);	/* gx_gfx3d, gx_cxo */
	branch_on(GPUCC + 0x919c);					/* cx_gfx3d */
	branch_on(GPUCC + 0x91a0);					/* cx_gfx3d_slv */
	con_hex2("CORE CLK", "on=", core, "faults=", bad);
	if (!core || bad) {
		con_str("GPU", "gfx3d clock not running - GPU registers NOT touched");
		return -1;
	}
	return 0;
}

static int gpu_alive(void)
{
	/* first contact with the GPU itself */
	uint32_t st = R(GREG(0x210));
	con_hex2("RBBM", "status=", st, "faults=", bad);
	if (bad) {
		con_str("GPU", "GPU register read faulted - stop");
		return -1;
	}
	W(GREG(0x883), 0x12345678);
	W(GREG(0x884), 0x9abcdef0);
	uint32_t s0 = R(GREG(0x883)), s1 = R(GREG(0x884));
	con_hex2("CP SCRATCH", "0=", s0, "1=", s1);
	con_hex2("CP", "sqe_cntl=", R(GREG(0x808)), "secvid=", R(GREG(0xf400)));
	con_hex2("RBBM", "int0=", R(GREG(0x201)), "faults=", bad);
	int ok = s0 == 0x12345678 && s1 == 0x9abcdef0;
	con_str("GPU", ok ? "ADRENO 613 ALIVE (registers respond)" : "GPU registers do not hold values");
	return ok ? 0 : -1;
}

/* ---------------- stage 8b: SMMU survey + first command stream ---------------- */

#define SMMU	0x03da0000UL
extern const uint8_t gpu_fw_start[], gpu_fw_end[];

static uint32_t sqe[16384] __attribute__((aligned(4096)));	/* 64 KiB */
static uint32_t ring[8192] __attribute__((aligned(32768)));	/* 32 KiB */
static volatile uint32_t scratch[1024] __attribute__((aligned(4096)));


/* stage-1 identity map, 1 GiB blocks, 39-bit VA (3-level, start at L1) */
static uint64_t smmu_l1[512] __attribute__((aligned(4096)));

static int smmu_identity_cb0(uintptr_t cb, uint32_t psz, uint32_t smr0)
{
	(void)psz;
	for (int i = 0; i < 512; i++)
		smmu_l1[i] = 0;
	for (uint64_t g = 0; g < 16; g++)		/* PA 0 .. 16 GiB */
		smmu_l1[g] = (g << 30) | (1u << 10) | (3u << 8) | (1u << 6) | 1;	/* AF, inner-sh, AP=RW all, block, attr0 */
	__asm__ volatile("dsb sy" ::: "memory");
	uintptr_t c = cb;				/* context bank 0 */
	W(c + 0x0, 0);					/* SCTLR off while programming */
	W(SMMU + 0x1000 + 0x800, 1);			/* CBA2R0: VA64 */
	W(SMMU + 0x1000, 1u << 16);			/* CBAR0: S1 translate, S2 bypass */
	W(c + 0x38, 0x44);				/* MAIR0 attr0 = normal non-cacheable */
	W(c + 0x3c, 0);
	W(c + 0x10, 2);					/* TCR2: PASize 40 bit */
	W(c + 0x30, 25 | (1u << 23));			/* TCR: T0SZ 25, TG0 4K, walks NC, EPD1 */
	W(c + 0x20, (uint32_t)(uintptr_t)smmu_l1);	/* TTBR0 */
	W(c + 0x24, (uint32_t)((uintptr_t)smmu_l1 >> 32));
	W(c + 0x618, 0);				/* TLBIALL */
	W(c + 0x7f0, 0);				/* TLBSYNC */
	for (int i = 0; i < 1000 && (R(c + 0x7f4) & 1); i++)
		udelay(1);
	W(c + 0x0, (1u << 6) | (1u << 5) | (1u << 2) | (1u << 1) | 1);	/* CFIE CFRE AFE TRE M */
	W(SMMU + 0xc00, 0);				/* S2CR0: translate, CB 0 */
	W(SMMU + 0x800, smr0);
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("SMMU cb0", "cbar=", R(SMMU + 0x1000), "sctlr=", R(c));
	con_hex2("SMMU cb0", "ttbr0=", R(c + 0x20), "tcr=", R(c + 0x30));
	con_hex2("SMMU map", "smr0=", R(SMMU + 0x800), "s2cr0=", R(SMMU + 0xc00));
	int ok = !bad && R(SMMU + 0x800) == smr0 && (R(SMMU + 0xc00) & 0x300ff) == 0 && (R(c) & 1) &&
		 R(c + 0x20) == (uint32_t)(uintptr_t)smmu_l1;
	con_str("SMMU", ok ? "identity context bank 0 installed" : "hypervisor refused SMMU programming");
	return ok;
}

static int smmu_bypass(void)
{
	uint32_t cr0 = R(SMMU + 0x0), idr0 = R(SMMU + 0x20), idr1 = R(SMMU + 0x24);
	con_hex2("SMMU", "cr0=", cr0, "idr1=", idr1);
	con_hex2("SMMU", "idr0=", idr0, "faults=", bad);
	if (bad)
		return 0;
	uint32_t psz = (idr1 >> 31) ? 0x10000 : 0x1000;
	uint32_t npage = 1u << (((idr1 >> 28) & 7) + 1);
	uintptr_t cb = SMMU + (uintptr_t)npage * psz;
	uint32_t nsmr = idr0 & 0xff;
	if (nsmr > 7)
		nsmr = 7;
	int matched = 0, matched_bypass = 0;
	for (uint32_t i = 0; i < nsmr; i++) {
		uint32_t smr = R(SMMU + 0x800 + 4 * i), s2cr = R(SMMU + 0xc00 + 4 * i);
		uint32_t cbndx = s2cr & 0xff, type = (s2cr >> 16) & 3;
		uint32_t sctlr = (type == 0 && cbndx < 8 && cb + (uintptr_t)cbndx * psz < SMMU + 0x10000) ?
				 R(cb + (uintptr_t)cbndx * psz) : 0xffffffff;
		(void)0;
		if (smr >> 31) {		/* valid entry: id [15:0], mask [31:16] style (v2: mask [30:16]) */
			uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
			if (((0 ^ id) & ~mask & 0x7fff) == 0) {	/* matches stream 0 */
				matched = 1;
				matched_bypass = type == 1 || (type == 0 && sctlr != 0xffffffff && !(sctlr & 1));
				con_hex2("  sid0 hit", "type=", type, "cb sctlr=", sctlr);
			}
		}
	}
	int clientpd = cr0 & 1, usfcfg = (cr0 >> 10) & 1;
	int ok = clientpd || (matched ? matched_bypass : !usfcfg);
	if (!ok && !matched) {
		/* kgsl gfx3d_user = <&kgsl_smmu 0x0 0x400>: SID 0, mask 0x400 */
		const uint32_t smr0 = 0x80000000u | (0x400u << 16);
		/* (a) bypass */
		W(SMMU + 0xc00, 0x00010000);
		W(SMMU + 0x800, smr0);
		__asm__ volatile("dsb sy" ::: "memory");
		con_hex2("SMMU bypass", "smr0=", R(SMMU + 0x800), "s2cr0=", R(SMMU + 0xc00));
		if (R(SMMU + 0x800) == smr0 && ((R(SMMU + 0xc00) >> 16) & 3) == 1) {
			con_str("SMMU", "bypass rule for SID 0 installed");
			return 1;
		}
		/* (b) translate through context bank 0 with an identity stage-1 table */
		return smmu_identity_cb0(cb, psz, smr0);
	}
	con_str("SMMU", clientpd ? "client port disabled: bypass" :
		ok ? "GPU stream 0 bypasses translation" : "GPU stream is TRANSLATED or faults - CP test skipped");
	return ok;
}

static uint32_t parity(uint32_t v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}

static uint32_t pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (parity(cnt) << 15) | ((op & 0x7f) << 16) | (parity(op) << 23);
}

static void W64(uint32_t reg, uint64_t v)
{
	W(GREG(reg), (uint32_t)v);
	W(GREG(reg + 1), (uint32_t)(v >> 32));
}

static void cp_test(void)
{
	uint32_t fwlen = gpu_fw_end - gpu_fw_start;
	if (fwlen < 1024 || fwlen - 4 > sizeof(sqe)) {
		con_hex("SQE fw missing, len", fwlen);
		return;
	}
	const volatile uint8_t *f = gpu_fw_start;
	for (uint32_t i = 0; i + 4 < fwlen + 4 && 4 + i < fwlen; i += 4)
		sqe[i / 4] = f[4 + i] | f[5 + i] << 8 | f[6 + i] << 16 | (uint32_t)f[7 + i] << 24;
	for (unsigned i = 0; i < 1024; i++)
		scratch[i] = 0;
	con_hex2("SQE fw", "len=", fwlen, "ver=", sqe[1]);

	/* hw_init (A610 family values) */
	W(GREG(0x3c45), 0); R(GREG(0x3c45));
	W(GREG(0x16), 0); R(GREG(0x16));
	W(GREG(0xf803), 0);
	W64(0xf800, 0);
	W(GREG(0xf802), 0);
	static const uint32_t am[] = {0x842, 0xc01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601, 0xe00, 0xae01, 0xb601, 0xf810};
	for (unsigned i = 0; i < sizeof(am) / 4; i++)
		W(GREG(am[i]), 1);
	for (unsigned i = 0; i < 4; i++)
		W(GREG(0x3c03 + i), 0x00071620);
	W(GREG(0x11), 3);
	W64(0xe05, 0x0001ffffffffffc0ULL);
	W64(0xe09, 0x0001fffffffff000ULL);
	W64(0xe07, 0x0001fffffffff000ULL);
	W64(0xe0b, 0x100000);
	W64(0xe0d, 0x100000 + 0x21000 - 1);
	W(GREG(0xe18), 0x804);
	W(GREG(0xe17), 0x4);
	W(GREG(0x8c2), 0x00800060);
	W(GREG(0x8c1), 0x40201b16);
	W(GREG(0x8c3), 48);
	W(GREG(0x90e), 47);
	W(GREG(0x9e00), 0x00080000);
	W(GREG(0x98d), 1);
	W(GREG(0x1f), (1u << 30) | 0x3ffff);
	W(GREG(0xe19), (1u << 7) | 1);

	W64(0x830, (uintptr_t)sqe);			/* CP_SQE_INSTR_BASE */
	W64(0x800, (uintptr_t)ring);			/* CP_RB_BASE */
	W(GREG(0x802), 12 | (2u << 8) | 0x08000000);	/* BUFSZ 32K, BLKSZ 32, NO_UPDATE */
	W(GREG(0x807), 0);
	con_hex2("HW INIT", "faults=", bad, "rbbm=", R(GREG(0x210)));
	if (bad)
		return;
	W(GREG(0x808), 1);				/* CP_SQE_CNTL: start the SQE */

	uint32_t n = 0;
	ring[n++] = pkt7(72, 8);			/* CP_ME_INIT */
	ring[n++] = 0x2f; ring[n++] = 3; ring[n++] = 0x20000000;
	ring[n++] = 0; ring[n++] = 0; ring[n++] = 0; ring[n++] = 0; ring[n++] = 0;
	ring[n++] = pkt7(61, 3);			/* CP_MEM_WRITE */
	ring[n++] = (uint32_t)(uintptr_t)scratch;
	ring[n++] = (uint32_t)((uintptr_t)scratch >> 32);
	ring[n++] = 0xC0FFEE01;
	ring[n++] = pkt7(16, 0);			/* CP_NOP pad */
	__asm__ volatile("dsb sy" ::: "memory");
	W(GREG(0x807), n);				/* CP_RB_WPTR */

	uint64_t end = ticks() + 19200ULL * 500;
	while (ticks() < end && scratch[0] != 0xC0FFEE01)
		;
	con_hex2("CP", "rptr=", R(GREG(0x806)), "wptr=", n);
	con_hex2("CP", "hwfault=", R(GREG(0x821)), "int=", R(GREG(0x823)));
	con_hex2("RBBM", "status=", R(GREG(0x210)), "int0=", R(GREG(0x201)));
	con_hex2("MEM", "scratch=", scratch[0], "faults=", bad);
	con_hex2("SMMU cb0", "fsr=", R(SMMU + 0x8000 + 0x58), "far=", R(SMMU + 0x8000 + 0x60));
	W(GREG(0xf400), 0);				/* leave secure mode (no zap) */
	con_hex("SECVID after", R(GREG(0xf400)));
	con_str("GPU", scratch[0] == 0xC0FFEE01 ? "GPU EXECUTED COMMANDS AND WROTE MEMORY" :
		"no memory write from the GPU");
}

void test_gpu(void)
{
	con_str("SHIM", "ios-flame stage 8d: Adreno 613 first commands");
	con_skip();
	if (gpu_power() || gpu_alive())
		return;
	con_skip();
	if (smmu_bypass())
		cp_test();
}
