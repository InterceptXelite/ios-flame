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
static uint8_t last_rx[24];

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

/* CS is never toggled by the engine itself (Linux spi-geni does the same):
 * explicit SPI_CS_ASSERT (8) / SPI_CS_DEASSERT (9) commands around each frame. */
static int spi_cmd(uint32_t op)
{
	wr(SE + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SE + SE_GENI_M_CMD0, op << 27);
	uint64_t end = ticks() + 19200 * 10;
	while (ticks() < end)
		if (rd(SE + SE_GENI_M_IRQ_STATUS) & 1) {
			wr(SE + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
			return 0;
		}
	con_hex2("SPI CS TIMEOUT", "op=", op, "irq=", rd(SE + SE_GENI_M_IRQ_STATUS));
	return -1;
}

static int spi_frame(const uint8_t *tx, uint8_t *rx, uint32_t n);

static int spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
	if (spi_cmd(8))
		return -1;
	int r = spi_frame(tx, rx, n);
	spi_cmd(9);
	udelay(5);
	return r;
}

/*
 * One frame, streaming through the FIFOs so it can be longer than they are.
 * The bytes sent are: hdr[0..nh-1] then data[0..n-nh-1] (data may be NULL = zeros).
 * rx (may be NULL) receives all n bytes (full duplex); without rx it is TX only.
 */
static int spi_stream(const uint8_t *hdr, uint32_t nh, const volatile uint8_t *data,
		      uint8_t *rx, uint32_t n)
{
	uint32_t op = rx ? 3 : 1;
	wr(SE + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SE + SE_SPI_TX_LEN, n);
	if (rx)
		wr(SE + SE_SPI_RX_LEN, n);
	wr(SE + SE_GENI_M_CMD0, (op << 27) | (1u << 2));	/* 3 = full duplex, 1 = TX only; FRAGMENTATION */
	uint32_t ti = 0, got = 0;
	uint64_t end = ticks() + 19200 * (50 + n / 256);
	while (ticks() < end) {
		while (ti < n && (rd(SE + SE_GENI_TX_FIFO_ST) & 0x0fffffff) < fifo_words) {
			uint32_t w = 0;
			for (uint32_t k = 0; k < 4 && ti < n; k++, ti++) {
				uint8_t c = ti < nh ? hdr[ti] : data ? data[ti - nh] : 0;
				w |= (uint32_t)c << (8 * k);
			}
			wr(SE + SE_GENI_TX_FIFO, w);
		}
		if (rx) {
			uint32_t wc = rd(SE + SE_GENI_RX_FIFO_ST) & 0x1ffffff;
			while (wc-- && got < n) {
				uint32_t w = rd(SE + SE_GENI_RX_FIFO);
				for (uint32_t k = 0; k < 4 && got < n; k++)
					rx[got++] = w >> (8 * k);
			}
		}
		uint32_t irq = rd(SE + SE_GENI_M_IRQ_STATUS);
		if ((irq & 1) && ti >= n && (!rx || got >= n)) {
			wr(SE + SE_GENI_M_IRQ_CLEAR, irq);
			return 0;
		}
	}
	con_hex2("SPI TIMEOUT", "irq=", rd(SE + SE_GENI_M_IRQ_STATUS), "got=", got);
	con_hex2("SPI ST", "n=", n, "ti=", ti);
	return -1;
}

static int spi_frame(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
	return spi_stream(tx, n, 0, rx, n);
}

static int nvt_write(uint8_t *buf, uint32_t n)
{
	uint8_t rx[24];
	buf[0] |= 0x80;
	return spi_xfer(buf, rx, n);
}

/* like CTP_SPI_READ: buf[0]=addr; returns buf[1..n-1] */
static int nvt_read(uint8_t *buf, uint32_t n)
{
	uint8_t tx[24] = {0}, rx[24] = {0};
	tx[0] = buf[0] & 0x7f;
	if (spi_xfer(tx, rx, n + 1))
		return -1;
	for (uint32_t i = 0; i < 24; i++)
		last_rx[i] = rx[i];
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


static uint32_t le4(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

/* protocol self-test: write a pattern to SRAM, read it back; dump the trim area */
static void __attribute__((unused)) nvt_rw_test(const char *tag)
{
	nvt_write_addr(0x3F0FE, 0x69);			/* bootloader reset (NT36528A) */
	delay_ms(5);
	nvt_set_page(0x22D00);
	uint8_t w[9] = {0x22D00 & 0x7f, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
	nvt_write(w, 9);
	uint8_t r[9] = {0x22D00 & 0x7f};
	nvt_read(r, 9);
	con_hex2(tag, "ram0-3=", le4(r + 1), "ram4-7=", le4(r + 5));
	nvt_set_page(0x3F000);
	uint8_t t[17] = {0x3F000 & 0x7f};
	nvt_read(t, 17);
	con_hex2("  3F000", "0-3=", le4(t + 1), "4-7=", le4(t + 5));
	con_hex2("  3F008", "8-b=", le4(t + 9), "c-f=", le4(t + 13));
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
		con_hex2("NVT raw", "rx0-3=", last_rx[0] | last_rx[1] << 8 | last_rx[2] << 16 | (uint32_t)last_rx[3] << 24,
			 "rx4-7=", last_rx[4] | last_rx[5] << 8 | last_rx[6] << 16 | (uint32_t)last_rx[7] << 24);
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


/* ---- NT36528A firmware download (hw CRC flow of nt36xxx_fw_update.c) ---- */

extern const uint8_t touch_fw_start[], touch_fw_end[];

#define EVENT_BUF	0x22D00
#define BOOT_RDY	0x3F10D
#define ILM_DES		0x3F128
#define DLM_DES		0x3F12C
#define ILM_LEN		0x3F118
#define DLM_LEN		0x3F130
#define G_ILM_CRC	0x3F100
#define G_DLM_CRC	0x3F104
#define R_ILM_CRC	0x3F120
#define R_DLM_CRC	0x3F124
#define CRC_FLAG	0x3F133

struct part { uint32_t sram, size, bin, crc; };
static struct part parts[24];
static uint32_t nparts;

static uint32_t fw4(const volatile uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int fw_parse(const volatile uint8_t *fw, uint32_t fwsize)
{
	uint32_t end = fw4(fw);
	if ((fw[0x20] & 2) || end < 0x30 || end > 0x1000) {
		con_hex2("FW", "bad header end=", end, "casc=", fw[0x20]);
		return -1;
	}
	uint32_t info = 0;
	for (uint32_t pos = 0x30; pos < end; pos += 0x10)
		info++;
	uint32_t ovly = (fw[0x28] & 0x10) ? (fw[0x28] & 0x0f) : 0;
	nparts = 2 + info + ovly;
	if (nparts > 24)
		return -1;
	for (uint32_t i = 0; i < nparts; i++) {
		struct part *p = &parts[i];
		if (i < 2) {
			p->bin = fw4(fw + 12 * i);
			p->sram = fw4(fw + 4 + 12 * i);
			p->size = fw4(fw + 8 + 12 * i);
			p->crc = fw4(fw + 0x18 + 4 * i);
		} else {
			uint32_t pos = i < 2 + info ? 0x30 + 0x10 * (i - 2)
						    : parts[1].bin + 0x10 * (i - 2 - info);
			p->sram = fw4(fw + pos);
			p->size = fw4(fw + pos + 4);
			p->bin = fw4(fw + pos + 8);
			p->crc = fw4(fw + pos + 12);
		}
		if (p->bin + p->size > fwsize) {
			con_hex2("FW part too big", "i=", i, "bin=", p->bin);
			return -1;
		}
	}
	return 0;
}

static int nvt_write_sram(uint32_t sram, const volatile uint8_t *src, uint32_t size)
{
	while (size) {
		uint32_t len = size > 4096 ? 4096 : size;
		uint8_t hdr = (sram & 0x7f) | 0x80;
		if (nvt_set_page(sram))
			return -1;
		if (spi_cmd(8))
			return -1;
		int r = spi_stream(&hdr, 1, src, 0, len + 1);
		spi_cmd(9);
		udelay(5);
		if (r)
			return -1;
		sram += len;
		src += len;
		size -= len;
	}
	return 0;
}

static void nvt_crc_bank(uint32_t des, uint32_t sram, uint32_t lenaddr, uint32_t size,
			 uint32_t gaddr, uint32_t crc)
{
	nvt_set_page(des);
	uint8_t b[5];
	b[0] = des & 0x7f; b[1] = sram; b[2] = sram >> 8; b[3] = sram >> 16;
	nvt_write(b, 4);
	b[0] = lenaddr & 0x7f; b[1] = size; b[2] = size >> 8; b[3] = size >> 16;
	nvt_write(b, 4);
	b[0] = gaddr & 0x7f; b[1] = crc; b[2] = crc >> 8; b[3] = crc >> 16; b[4] = crc >> 24;
	nvt_write(b, 5);
}

static uint32_t nvt_rd32(uint32_t addr)
{
	nvt_set_page(addr);
	uint8_t b[5] = {addr & 0x7f};
	nvt_read(b, 5);
	return b[1] | b[2] << 8 | b[3] << 16 | (uint32_t)b[4] << 24;
}

static int nvt_download(void)
{
	const volatile uint8_t *fw = touch_fw_start;
	uint32_t fwsize = touch_fw_end - touch_fw_start;
	con_hex2("FW", "size=", fwsize, "ver=", fwsize >= 0x1000 ? fw[fwsize - 0x1000] : 0);
	if (fwsize < 0x10000 || fw_parse(fw, fwsize))
		return -1;
	con_hex2("FW ILM", "size=", parts[0].size, "crc=", parts[0].crc);
	con_hex2("FW DLM", "size=", parts[1].size, "crc=", parts[1].crc);
	con_hex("FW parts", nparts);

	for (int attempt = 0; attempt < 3; attempt++) {
		uint64_t t0 = ticks();
		nvt_write_addr(0x3F0FE, 0x69);		/* bootloader reset */
		delay_ms(5);
		nvt_crc_bank(ILM_DES, parts[0].sram, ILM_LEN, parts[0].size, G_ILM_CRC, parts[0].crc);
		nvt_crc_bank(DLM_DES, parts[1].sram, DLM_LEN, parts[1].size, G_DLM_CRC, parts[1].crc);
		int bad = 0;
		for (uint32_t i = 0; i < nparts && !bad; i++)
			if (parts[i].size)
				bad = nvt_write_sram(parts[i].sram, fw + parts[i].bin, parts[i].size + 1);
		if (bad) {
			con_str("FW", "SRAM write failed");
			continue;
		}
		/* clear reset status, enable fw crc */
		nvt_set_page(EVENT_BUF);
		uint8_t c[7] = {0x60, 0, 0, 0, 0, 0, 0};
		nvt_write(c, 7);
		uint8_t e[3] = {0x50, 0xAE, 0x00};
		nvt_write(e, 3);
		nvt_write_addr(BOOT_RDY, 1);		/* boot ready */
		delay_ms(5);
		uint32_t us = (uint32_t)((ticks() - t0) * 10 / 192);
		/* wait for RESET_STATE_INIT (0xA0..0xAF) */
		uint8_t st[6] = {0};
		for (int i = 0; i < 50; i++) {
			nvt_set_page(EVENT_BUF | 0x60);
			st[0] = 0x60;
			nvt_read(st, 6);
			if (st[1] >= 0xA0 && st[1] <= 0xAF)
				break;
			delay_ms(10);
		}
		con_hex2("FW BOOT", "state=", st[1], "us=", us);
		if (st[1] >= 0xA0 && st[1] <= 0xAF)
			return 0;
		nvt_set_page(CRC_FLAG);
		uint8_t f[2] = {CRC_FLAG & 0x7f};
		nvt_read(f, 2);
		con_hex2("FW CRC", "flag=", f[1], "ilm=", nvt_rd32(R_ILM_CRC));
		con_hex2("FW CRC", "g_ilm=", nvt_rd32(G_ILM_CRC), "dlm=", nvt_rd32(R_DLM_CRC));
	}
	return -1;
}

static void dot(uint32_t x, uint32_t y, uint32_t color)
{
	if (x >= FB_W || y >= FB_H)
		return;
	for (uint32_t dy = 0; dy < 6; dy++)
		for (uint32_t dx = 0; dx < 6; dx++)
			if (x + dx < FB_W && y + dy < FB_H)
				con_fb[(y + dy) * (FB_STRIDE / 4) + x + dx] = color;
}

/* poll the event buffer for ~2 minutes and draw every finger */
static void nvt_touch_loop(void)
{
	uint8_t fwi[12] = {0x78};
	nvt_set_page(EVENT_BUF | 0x78);
	nvt_read(fwi, 12);
	con_hex2("FW INFO", "ver=", fwi[1], "x/y max=", (fwi[5] << 8 | fwi[6]) << 16 | (fwi[7] << 8 | fwi[8]));
	con_str("TOUCH", "DRAW ON THE SCREEN NOW (2 min)");
	uint32_t pk = 0, bad = 0, fingers = 0;
	static uint8_t tx[80], rx[80];
	uint64_t end = ticks() + 19200ULL * 1000 * 120;
	while (ticks() < end) {
		nvt_set_page(EVENT_BUF);
		tx[0] = 0;			/* read offset 0 of the event buffer */
		if (spi_xfer(tx, rx, 67))
			break;
		uint8_t *d = rx + 1;		/* d[1..65] = point_data[1..65] */
		pk++;
		uint8_t sum = 0;
		for (int i = 1; i < 65; i++)
			sum += d[i];
		sum = ~sum + 1;
		if (sum != d[65]) {
			bad++;
			delay_ms(8);
			continue;
		}
		for (int i = 0; i < 10; i++) {
			uint8_t *p = d + 1 + 6 * i;
			uint8_t id = p[0] >> 3, s = p[0] & 7;
			if (id == 0 || id > 10 || (s != 1 && s != 2))
				continue;
			uint32_t x = (p[1] << 4) | (p[3] >> 4);
			uint32_t y = (p[2] << 4) | (p[3] & 0xf);
			dot(x, y, id == 1 ? 0xff00ff00 : 0xffff00ff);
			fingers++;
		}
		delay_ms(8);
	}
	con_hex2("TOUCH END", "packets=", pk, "badsum=", bad);
	con_hex("TOUCH dots", fingers);
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
			if (((perf >> 8) & 7) == 0 && (perf & 0x1f) <= 1)
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
	con_str("SHIM", "ios-flame stage 6g: touch firmware + points");
	con_skip();
	find_panel(dtb);
	int novatek = has(panel, "c3f_35") || has(panel, "c3f_37") || has(panel, "c3f_43");
	con_str("CHIP", novatek ? "panel says Novatek NT36528" :
		has(panel, "c3f_46") ? "panel says FocalTech FT8057M (HKC)" : "unknown panel");
	con_skip();

	con_hex("GCC vote", rd(GCC_VOTE));
	con_hex2("CBCR ahb", "m=", rd(CBCR_M_AHB), "s=", rd(CBCR_S_AHB));
	con_hex2("CBCR s1", "cbcr=", rd(CBCR_S1), "rcgcmd=", rd(RCG_S1));
	con_hex2("RCG s1", "cfg=", rd(RCG_S1 + 4), "dfsr=", rd(RCG_S1 + 0x14));
	con_hex2("GPIO98 rst", "ctl=", rd(TLMM_CTL(98)), "io=", rd(TLMM_IO(98)));
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
		/* driver probe: eng reset, then TP_RESX (gpio98) high, 10 ms after POR */
		nvt_write_addr(0x7FFF80, 0x5A);
		delay_ms(1);
		wr(TLMM_IO(98), 2);			/* output value high */
		wr(TLMM_CTL(98), (1u << 9) | (3u << 6));	/* gpio, OE, 8 mA, no pull */
		delay_ms(10);
		con_hex2("GPIO98 rst", "ctl=", rd(TLMM_CTL(98)), "io=", rd(TLMM_IO(98)));
		/* irq gpio34: input, no pull (the touch IC drives it) */
		wr(TLMM_CTL(34), 0);
		nvt_probe();
		if (nvt_download() == 0)
			nvt_touch_loop();
		else
			con_str("FW", "download FAILED");
	} else {
		con_str("TOUCH", "not Novatek: SPI probe skipped this round");
	}
}
