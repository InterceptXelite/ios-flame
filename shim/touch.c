/*
 * flame stage 6b: touch bring-up diagnostics (shim only, iOS is not booted).
 *
 * Real wiring (stock dtbo fragment@89 / qupv3_se1_spi):
 *   QUP0 SE1 GENI SPI @ 0x984000, CS0, mode 0; pins gpio10..13 (MISO MOSI CLK CS)
 *   touch irq gpio34, reset gpio98
 *   NT36528 (Novatek, panels c3f_35/37/43) or FT8057M (FocalTech, HKC panel)
 *
 * Steps (each printed): panel name from ABL bootargs; GCC/TLMM state; vote the
 * QUP0 clocks on; SE1 firmware protocol; if SPI + Novatek panel: bootloader
 * reset + chip id read, exactly what the Android driver does at probe.
 * Writes: GCC clock vote, TLMM mux of gpio10..13, SE1 registers, and two
 * Novatek SPI commands. Nothing touches PMIC, display or storage.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "touch.h"

extern int probe_wr32(uintptr_t a, uint32_t v);
extern void delay_ms(unsigned ms);

static inline uint32_t rd(uintptr_t a) { uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline void wr(uintptr_t a, uint32_t v) { __asm__ volatile("dsb sy; str %w0, [%1]; dsb sy" : : "r"(v), "r"(a) : "memory"); }
static uint64_t ticks(void) { uint64_t t; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t)); return t; }
static void udelay(unsigned us) { uint64_t e = ticks() + (uint64_t)us * 192 / 10; while (ticks() < e) ; }

#define GCC		0x00100000UL
#define GCC_VOTE	(GCC + 0x62008)	/* APCS clock branch enable vote */
#define VOTE_QUP0	((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 11))
#define CBCR_M_AHB	(GCC + 0x27004)
#define CBCR_S_AHB	(GCC + 0x27008)
#define CBCR_CORE	(GCC + 0x33000)
#define CBCR_CORE2X	(GCC + 0x3300c)
#define CBCR_S1		(GCC + 0x27140)
#define RCG_S1		(GCC + 0x27148)	/* CMD; +4 CFG; +0x14 DFSR; +0x1c+4n PERF */

#define TLMM		0x0f100000UL
#define TLMM_CTL(n)	(TLMM + 0x1000UL * (n))
#define TLMM_IO(n)	(TLMM_CTL(n) + 4)

#define QUP0		0x009c0000UL
#define SE		0x00984000UL
#define GENI_FORCE_DEFAULT	0x20
#define GENI_OUTPUT_CTRL	0x24
#define GENI_CGC_CTRL		0x28
#define SE_GENI_STATUS		0x40
#define GENI_SER_M_CLK_CFG	0x48
#define GENI_IF_DISABLE_RO	0x64
#define GENI_FW_REVISION_RO	0x68
#define SE_GENI_CLK_SEL		0x7c
#define SE_SPI_CPHA		0x224
#define SE_SPI_LOOPBACK		0x22c
#define SE_SPI_CPOL		0x230
#define SE_SPI_DEMUX_INV	0x24c
#define SE_SPI_DEMUX_SEL	0x250
#define SE_GENI_BYTE_GRAN	0x254
#define SE_GENI_DMA_MODE_EN	0x258
#define SE_SPI_TRANS_CFG	0x25c
#define SE_GENI_TX_PACK0	0x260
#define SE_GENI_TX_PACK1	0x264
#define SE_SPI_WORD_LEN		0x268
#define SE_SPI_TX_LEN		0x26c
#define SE_SPI_RX_LEN		0x270
#define SE_GENI_RX_PACK0	0x284
#define SE_GENI_RX_PACK1	0x288
#define SE_GENI_M_CMD0		0x600
#define SE_GENI_M_IRQ_STATUS	0x610
#define SE_GENI_M_IRQ_EN	0x614
#define SE_GENI_M_IRQ_CLEAR	0x618
#define SE_GENI_S_IRQ_CLEAR	0x648
#define SE_GENI_TX_FIFO		0x700
#define SE_GENI_RX_FIFO		0x780
#define SE_GENI_TX_FIFO_ST	0x800
#define SE_GENI_RX_FIFO_ST	0x804
#define SE_DMA_TX_IRQ_CLR	0xc44
#define SE_DMA_RX_IRQ_CLR	0xd44
#define SE_GSI_EVENT_EN		0xe18
#define SE_IRQ_EN		0xe1c
#define SE_HW_PARAM_0		0xe24
#define SE_DMA_GENERAL_CFG	0xe30

static char panel[48];
static uint32_t fifo_words;

static int has(const char *s, const char *sub)
{
	for (; *s; s++) {
		const char *a = s, *b = sub;
		while (*b && *a == *b)
			a++, b++;
		if (!*b)
			return 1;
	}
	return 0;
}

static void find_panel(uintptr_t dtb)
{
	uintptr_t v;
	uint32_t len;
	static const char key[] = "mdss_dsi_panel_";
	panel[0] = 0;
	if (!fdt_chosen_prop(dtb, "bootargs", &v, &len)) {
		con_str("PANEL", "no /chosen/bootargs");
		return;
	}
	const volatile char *s = (const volatile char *)v;
	for (uint32_t i = 0; i + sizeof(key) < len; i++) {
		uint32_t k = 0;
		while (k < sizeof(key) - 1 && s[i + k] == key[k])
			k++;
		if (k != sizeof(key) - 1)
			continue;
		uint32_t j = 0;
		for (i += k; i < len && j < sizeof(panel) - 1; i++, j++) {
			char c = s[i];
			if (c == 0 || c == ' ' || c == ':' || c == ',')
				break;
			panel[j] = c;
		}
		panel[j] = 0;
		break;
	}
	con_str("PANEL", panel[0] ? panel : "(not in bootargs)");
	con_hex("BOOTARGS len", len);
}

/* ---- GENI SPI, FIFO mode, polled ---- */

static int spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
	if (n == 0 || n > fifo_words * 4)
		return -1;
	wr(SE + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SE + SE_SPI_TX_LEN, n);
	wr(SE + SE_SPI_RX_LEN, n);
	wr(SE + SE_GENI_M_CMD0, 7u << 27);		/* SPI_TX_RX, no fragmentation */
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = 0;
		for (uint32_t b = 0; b < 4 && i + b < n; b++)
			w |= (uint32_t)tx[i + b] << (8 * b);
		wr(SE + SE_GENI_TX_FIFO, w);
	}
	uint32_t got = 0;
	uint64_t end = ticks() + 19200 * 50;		/* 50 ms */
	while (ticks() < end) {
		uint32_t st = rd(SE + SE_GENI_RX_FIFO_ST);
		uint32_t wc = st & 0x1ffffff;
		while (wc-- && got < n) {
			uint32_t w = rd(SE + SE_GENI_RX_FIFO);
			for (uint32_t b = 0; b < 4 && got < n; b++)
				rx[got++] = w >> (8 * b);
		}
		uint32_t irq = rd(SE + SE_GENI_M_IRQ_STATUS);
		if ((irq & 1) && got >= n) {
			wr(SE + SE_GENI_M_IRQ_CLEAR, irq);
			return 0;
		}
	}
	con_hex2("SPI TIMEOUT", "irq=", rd(SE + SE_GENI_M_IRQ_STATUS), "got=", got);
	con_hex2("SPI ST", "geni=", rd(SE + SE_GENI_STATUS), "rxst=", rd(SE + SE_GENI_RX_FIFO_ST));
	return -1;
}

static int nvt_write(uint8_t *buf, uint32_t n)
{
	uint8_t rx[16];
	buf[0] |= 0x80;
	return spi_xfer(buf, rx, n);
}

/* like CTP_SPI_READ: buf[0]=addr; returns buf[1..n-1] */
static int nvt_read(uint8_t *buf, uint32_t n)
{
	uint8_t tx[16] = {0}, rx[16] = {0};
	tx[0] = buf[0] & 0x7f;
	if (spi_xfer(tx, rx, n + 1))
		return -1;
	for (uint32_t i = 1; i < n; i++)
		buf[i] = rx[i + 1];
	return 0;
}

static int nvt_set_page(uint32_t a)
{
	uint8_t b[3] = {0xff, (a >> 15) & 0xff, (a >> 7) & 0xff};
	return nvt_write(b, 3);
}

static int nvt_write_addr(uint32_t a, uint8_t v)
{
	uint8_t b[2] = {a & 0x7f, v};
	if (nvt_set_page(a))
		return -1;
	return nvt_write(b, 2);
}

static void nvt_probe(void)
{
	static const uint32_t regs[3][2] = {
		{0x1FB104, 0x1FB43E}, {0x3F004, 0x3F0FE}, {0x1F64E, 0x1F01A},
	};
	for (int r = 0; r < 3; r++) {
		uint32_t trim = regs[r][0], swrst = regs[r][1];
		if (nvt_write_addr(swrst, 0x69))	/* bootloader reset */
			return;
		delay_ms(5);
		nvt_set_page(trim);
		uint8_t b[7] = {trim & 0x7f, 0, 0, 0, 0, 0, 0};
		nvt_write(b, 7);
		uint8_t id[7] = {trim & 0x7f, 0, 0, 0, 0, 0, 0};
		if (nvt_read(id, 7))
			return;
		con_hex2(r == 0 ? "NVT ID 1FB104" : r == 1 ? "NVT ID 3F004" : "NVT ID 1F64E",
			 "b1-4=", id[1] | id[2] << 8 | id[3] << 16 | (uint32_t)id[4] << 24,
			 "b5-6=", id[5] | id[6] << 8);
		/* NT36528(A): x x x 28 65 03 */
		if (id[4] == 0x28 && id[5] == 0x65 && id[6] == 0x03) {
			con_str("TOUCH", id[1] == 0x0a ? "NT36528A FOUND" : "NT36528 FOUND");
			return;
		}
	}
	con_str("TOUCH", "no Novatek id matched");
}

static void pin_spi(void)
{
	/* func 1 (qup0_se1_lN), 6 mA (2<<6); MISO/CS pull-up (3), MOSI/CLK pull-down (1) */
	static const uint8_t pull[4] = {3, 1, 1, 3};
	for (int i = 0; i < 4; i++)
		wr(TLMM_CTL(10 + i), (1u << 2) | (2u << 6) | pull[i]);
}

static int se_spi_setup(void)
{
	uint32_t fw = rd(SE + GENI_FW_REVISION_RO);
	uint32_t proto = (fw >> 8) & 0xff;
	uint32_t ifdis = rd(SE + GENI_IF_DISABLE_RO);
	uint32_t hw0 = rd(SE + SE_HW_PARAM_0);
	con_hex2("SE1", "fwrev=", fw, "ifdis=", ifdis);
	con_hex2("SE1", "hwpar0=", hw0, "qupver=", rd(QUP0 + 4));
	if (proto != 1) {
		con_str("SE1", proto == 3 ? "firmware is I2C, not SPI - stop" : "firmware is not SPI - stop");
		return -1;
	}
	if (ifdis & 1) {
		con_str("SE1", "FIFO disabled (GPI DMA only) - stop");
		return -1;
	}
	fifo_words = (hw0 >> 16) & 0x3f;
	if (fifo_words == 0)
		fifo_words = 16;
	if (fifo_words > 3)
		fifo_words -= 1;	/* keep one word of headroom */

	/* geni_se_init + FIFO mode */
	wr(SE + SE_GSI_EVENT_EN, 0);
	wr(SE + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SE + SE_GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE + SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE + SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(SE + SE_IRQ_EN, 0xf);
	wr(SE + GENI_CGC_CTRL, rd(SE + GENI_CGC_CTRL) | 0x7f);
	wr(SE + SE_DMA_GENERAL_CFG, rd(SE + SE_DMA_GENERAL_CFG) | 0xf);
	wr(SE + GENI_OUTPUT_CTRL, 0x7f);
	wr(SE + GENI_FORCE_DEFAULT, 1);
	wr(SE + SE_GENI_DMA_MODE_EN, rd(SE + SE_GENI_DMA_MODE_EN) & ~1u);
	wr(SE + SE_GENI_M_IRQ_EN, rd(SE + SE_GENI_M_IRQ_EN) | 0x7e | 1 | (1u << 26) | (1u << 27) | (1u << 30));

	/* 8-bit words, 4 per FIFO word, MSB first (geni_se_config_packing) */
	uint32_t cfg[4];
	for (int i = 0; i < 4; i++)
		cfg[i] = ((7u + 8u * i) << 5) | (1u << 4) | (7u << 1);
	cfg[3] |= 1;
	uint32_t c0 = cfg[0] | cfg[1] << 10, c1 = cfg[2] | cfg[3] << 10;
	wr(SE + SE_GENI_TX_PACK0, c0);
	wr(SE + SE_GENI_TX_PACK1, c1);
	wr(SE + SE_GENI_RX_PACK0, c0);
	wr(SE + SE_GENI_RX_PACK1, c1);
	wr(SE + SE_GENI_BYTE_GRAN, 0);
	wr(SE + SE_SPI_WORD_LEN, 8 - 4);
	wr(SE + SE_SPI_LOOPBACK, 0);
	wr(SE + SE_SPI_DEMUX_SEL, 0);
	wr(SE + SE_SPI_CPHA, 0);
	wr(SE + SE_SPI_CPOL, 0);
	wr(SE + SE_SPI_DEMUX_INV, 0);
	wr(SE + SE_SPI_TRANS_CFG, rd(SE + SE_SPI_TRANS_CFG) & ~2u);

	/* clock: 19.2 MHz XO source, divider 4 -> 4.8 MHz (driver max is 9.6) */
	uint32_t idx = 0, dfs = rd(RCG_S1 + 0x14) & 1;
	if (dfs) {
		for (idx = 0; idx < 8; idx++) {
			uint32_t perf = rd(RCG_S1 + 0x1c + 4 * idx);
			if (((perf >> 8) & 7) == 0 && (perf & 0x1f) == 0)
				break;		/* XO, no divider */
		}
		if (idx == 8)
			idx = 0;
	}
	con_hex2("SE1 CLK", "dfs=", dfs, "level=", idx);
	uint32_t qv = rd(QUP0 + 4);
	uint32_t div = ((qv >> 28) == 1 && ((qv >> 16) & 0xfff) == 0) ? 2 : 4;	/* oversampling 2 on QUP 1.0 */
	wr(SE + SE_GENI_CLK_SEL, idx & 7);
	wr(SE + GENI_SER_M_CLK_CFG, (div << 4) | 1);
	con_hex2("SE1 RDY", "fifo=", fifo_words, "geni=", rd(SE + SE_GENI_STATUS));
	return 0;
}

void test_touch(uintptr_t dtb)
{
	con_str("SHIM", "ios-flame stage 6b: touch diagnostics");
	con_skip();
	find_panel(dtb);
	int novatek = has(panel, "c3f_35") || has(panel, "c3f_37") || has(panel, "c3f_43");
	con_str("CHIP", novatek ? "panel says Novatek NT36528" :
		has(panel, "c3f_46") ? "panel says FocalTech FT8057M (HKC)" : "unknown panel");
	con_skip();

	con_hex("GCC vote", rd(GCC_VOTE));
	con_hex2("CBCR ahb", "m=", rd(CBCR_M_AHB), "s=", rd(CBCR_S_AHB));
	con_hex2("CBCR core", "1x=", rd(CBCR_CORE), "2x=", rd(CBCR_CORE2X));
	con_hex2("CBCR s1", "cbcr=", rd(CBCR_S1), "rcgcmd=", rd(RCG_S1));
	con_hex2("RCG s1", "cfg=", rd(RCG_S1 + 4), "dfsr=", rd(RCG_S1 + 0x14));
	con_hex2("RCG perf", "l0=", rd(RCG_S1 + 0x1c), "l1=", rd(RCG_S1 + 0x20));
	con_hex2("RCG perf", "l2=", rd(RCG_S1 + 0x24), "l3=", rd(RCG_S1 + 0x28));
	static const uint8_t pins[6] = {10, 11, 12, 13, 34, 98};
	for (int i = 0; i < 6; i++) {
		const char *nm = i == 0 ? "GPIO10 miso" : i == 1 ? "GPIO11 mosi" : i == 2 ? "GPIO12 clk" :
				 i == 3 ? "GPIO13 cs" : i == 4 ? "GPIO34 irq" : "GPIO98 rst";
		con_hex2(nm, "ctl=", rd(TLMM_CTL(pins[i])), "io=", rd(TLMM_IO(pins[i])));
	}
	con_skip();

	uint32_t vote = rd(GCC_VOTE);
	if (!probe_wr32(GCC_VOTE, vote | VOTE_QUP0)) {
		con_str("GCC", "clock vote write ABORTED (hypervisor) - stop");
		return;
	}
	uint64_t end = ticks() + 19200 * 20;
	while ((rd(CBCR_S1) >> 31) && ticks() < end)
		;
	con_hex2("CLK ON", "vote=", rd(GCC_VOTE), "s1=", rd(CBCR_S1));
	if ((rd(CBCR_S1) >> 31) || (rd(CBCR_S_AHB) >> 31) || (rd(CBCR_M_AHB) >> 31)) {
		con_str("GCC", "QUP0 SE1 clocks still off - stop");
		return;
	}
	if (se_spi_setup())
		return;
	pin_spi();
	udelay(100);
	if (novatek) {
		nvt_probe();
	} else {
		con_str("TOUCH", "not Novatek: SPI probe skipped this round");
	}
	con_hex2("GPIO34 irq", "ctl=", rd(TLMM_CTL(34)), "io=", rd(TLMM_IO(34)));
	con_hex2("GPIO98 rst", "ctl=", rd(TLMM_CTL(98)), "io=", rd(TLMM_IO(98)));
}
