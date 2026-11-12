/*
 * Milestone 5 probe: can we talk to the internal UFS that ABL booted from?
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
 * is enabled and the link is up, points the transfer request list at our own
 * buffers and issues READ-ONLY SCSI commands on LUN 0. Nothing is ever
 * written to the device: there is no SCSI WRITE in this file.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#include "board.h"

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

static inline uint32_t ufs_rd(uint32_t off) { uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(UFS_BASE + off) : "memory"); return v; }
static inline void ufs_wr(uint32_t off, uint32_t v) { __asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(UFS_BASE + off) : "memory"); }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {	/* 1 s */
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[7];		/* SCSI status in the response UPIU */
	if (*ocs_out)
		return 2;
	if (ucd.resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

/* Sense data from the last response UPIU (fixed format): key / ASC / ASCQ. */
static void show_sense(const char *label)
{
	const uint8_t *r = ucd.resp;
	uint32_t slen = (uint32_t)r[32] << 8 | r[33];
	const uint8_t *sd = r + 34;
	con_hex2(label, "key/asc/q=", (uint32_t)(sd[2] & 0xf) << 16 | (uint32_t)sd[12] << 8 | sd[13], "len=", slen);
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (!__builtin_strcmp(name, "userdata") || !__builtin_strcmp(name, "cust") ||
		    !__builtin_strcmp(name, "cache"))
			con_str(" fs", fs_type((uint32_t)first));
	}
}

void test_ufs(void)
{
	con_str("UFS", "milestone 5 probe (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever ABL left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return;
	}

	uint32_t ocs = 0, st = 0, rc;
	static const uint8_t tur[6] = { 0x00 };
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
		show_sense("UFS SENSE");
	}

	/* flame (1st boot): every command answered CHECK CONDITION. If the LU is
	 * stopped/asleep after ABL, START STOP UNIT (power condition ACTIVE,
	 * START=1) wakes it; Linux sends the same on resume. No data is written. */
	if (rc) {
		/* flame: every LU answered NOT READY 04/02 (all 8, even INQUIRY):
		 * ABL leaves the whole device in PowerDown. The device power mode
		 * belongs to the "UFS Device" well-known LU (W-LUN 0x50, UPIU LUN
		 * 0xD0), as in Linux ufshcd_set_dev_pwr_mode(): SSU there with
		 * POWER CONDITION = 1 (ACTIVE). Clear its unit attention first. */
		const uint8_t WLUN_DEV = 0xd0;
		for (int i = 0; i < 2; i++) {
			uint32_t r1 = scsi(WLUN_DEV, tur, sizeof(tur), 0, 0, &ocs, &st);
			con_hex2("UFS WLUN TUR", "rc=", r1, "ocs/st=", ocs << 8 | st);
			if (!r1)
				break;
			show_sense("UFS SENSE");
		}
		static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };	/* ACTIVE */
		for (int i = 0; i < 3; i++) {
			uint32_t r2 = scsi(WLUN_DEV, ssu, sizeof(ssu), 0, 0, &ocs, &st);
			con_hex2("UFS WLUN SSU", "rc=", r2, "ocs/st=", ocs << 8 | st);
			if (!r2)
				break;
			show_sense("UFS SENSE");
		}
		for (int i = 0; i < 3; i++) {
			rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
			con_hex2("UFS TUR2", "rc=", rc, "ocs/st=", ocs << 8 | st);
			if (!rc)
				break;
			show_sense("UFS SENSE");
		}
	}

	/* Which LUNs exist and how big: INQUIRY never changes anything. */
	for (uint8_t lun = 0; lun < 4; lun++) {
		static const uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
		uint32_t r3 = scsi(lun, inq, sizeof(inq), data, 36, &ocs, &st);
		if (r3) {
			con_hex2("UFS LUN", "n=", lun, "inq rc=", r3 << 8 | st);
			continue;
		}
		static const uint8_t c10[10] = { 0x25 };
		r3 = scsi(lun, c10, sizeof(c10), data, 8, &ocs, &st);
		con_hex2("UFS LUN", "n=", lun, r3 ? "cap rc=" : "MiB=", r3 ? (r3 << 8 | st) : (uint32_t)(((uint64_t)be32(data) + 1) * be32(data + 4) >> 20));
	}

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	con_hex2("UFS CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));

	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = scsi(0, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st);
	con_hex2("UFS READ", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc)
		return;
	con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("UFS GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);
}

/* ======================================================================
 * Log viewer (FLAME_LOGVIEW): shows the tail of the sweetlog ring (patch
 * 0047, last 1025 blocks of userdata) on the panel. READ-ONLY like the rest
 * of this file. Volume up = older page, volume down = newer page.
 * ====================================================================== */
#include "font.h"

extern void delay_ms(unsigned ms);

#define LV_TAIL_BLOCKS	64			/* last 256 KiB of the console stream */
#define LV_COLS		88
#define LV_ROWS		100
#define LV_MAXLINES	12000

static uint8_t lv_buf[(LV_TAIL_BLOCKS + 1) * BLOCK] __attribute__((aligned(4096)));
static uint32_t lv_line_off[LV_MAXLINES];
static uint16_t lv_line_len[LV_MAXLINES];

static int ufs_open_quiet(void)
{
	uint32_t hce = ufs_rd(REG_HCE), hcs = ufs_rd(REG_HCS);
	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY) || ufs_rd(REG_UTRLDBR))
		return -1;
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1))
		return -2;
	uint32_t ocs, st, rc = 1;
	static const uint8_t tur[6] = { 0x00 };
	static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
	for (int i = 0; i < 3 && rc; i++)
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
	if (rc) {
		for (int i = 0; i < 2; i++)
			if (!scsi(0xd0, tur, sizeof(tur), 0, 0, &ocs, &st))
				break;
		for (int i = 0; i < 3; i++)
			if (!scsi(0xd0, ssu, sizeof(ssu), 0, 0, &ocs, &st))
				break;
		for (int i = 0; i < 3 && rc; i++)
			rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
	}
	return rc ? -3 : 0;
}

static int find_userdata(uint64_t *first, uint64_t *last)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	if (read_blocks(0, 2, data))
		return -1;
	const uint8_t *gpt = data + BLOCK;
	uint32_t nent = gpt[80] | gpt[81] << 8, elba = gpt[72] | gpt[73] << 8;
	if (nent > 128 || read_blocks(elba, (nent * 128 + BLOCK - 1) / BLOCK, ents))
		return -2;
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		static const char want[] = "userdata";
		int k = 0;
		while (k < 8 && e[56 + 2 * k] == (uint8_t)want[k])
			k++;
		if (k == 8 && e[56 + 16] == 0) {
			*first = le64(e + 32);
			*last = le64(e + 40);
			return 0;
		}
	}
	return -3;
}

static void lv_char(int col, int r, char ch, uint32_t fg, uint32_t bg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = ' ';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = 4 + col * 8, y0 = 8 + r * 16;
	if (x0 + 8 > FB_W || y0 + 16 > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++) {
		volatile uint32_t *p = con_fb + (y0 + gy) * (FB_STRIDE / 4) + x0;
		for (int gx = 0; gx < 8; gx++)
			p[gx] = (g[gy] & (0x80 >> gx)) ? fg : bg;
	}
}

static void lv_text(int col, int r, const char *s, uint32_t fg, uint32_t bg)
{
	while (*s && col < LV_COLS)
		lv_char(col++, r, *s++, fg, bg);
	while (col < LV_COLS)
		lv_char(col++, r, ' ', fg, bg);
}

static void lv_num(char *p, uint32_t v)
{
	char t[12];
	int n = 0;
	do {
		t[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		*p++ = t[--n];
	*p = 0;
}

/* keys, read-only: vol-up = TLMM gpio53 low; vol-down = PMK8350 PON RT_STS bit 6 */
#define LV_TLMM53	(0x0f100000UL + 53UL * 0x1000UL + 4)
#define LV_CORE		0x0c400000UL
#define LV_OBS		0x0c440000UL
static uint32_t lv_rd(uintptr_t a) { uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static void lv_wr(uintptr_t a, uint32_t v) { __asm__ volatile("dsb sy; str %w0, [%1]; dsb sy" : : "r"(v), "r"(a) : "memory"); }
static int lv_pon_apid = -1;

static void lv_keys_init(void)
{
	uint32_t ver = lv_rd(LV_CORE);
	if (ver < 0x70000000u || ver >= 0x80000000u)
		return;
	for (int i = 0; i < 1024; i++) {
		uint32_t map = lv_rd(LV_CORE + 0x2000 + 4 * i);
		if (map && ((map >> 8) & 0xfff) == 0x013) {
			lv_pon_apid = i;
			return;
		}
	}
}

static int lv_voldown(void)
{
	if (lv_pon_apid < 0)
		return 0;
	uintptr_t ch = LV_OBS + 0x20UL * lv_pon_apid;
	lv_wr(ch, (1u << 27) | (0x10u << 4));		/* EXT_READL 0x1310 low byte */
	for (int i = 0; i < 1000; i++) {
		uint32_t st = lv_rd(ch + 8);
		if (st & 1)
			return (st & 0xe) ? 0 : !!(lv_rd(ch + 0x18) & 0x40);
	}
	return 0;
}

static int lv_volup(void)
{
	return !(lv_rd(LV_TLMM53) & 1);
}

static void lv_show(uint32_t nlines, uint32_t top, const char *hdr)
{
	char h[96];
	int n = 0;
	for (const char *s = hdr; *s && n < 60; s++)
		h[n++] = *s;
	h[n++] = ' ';
	lv_num(h + n, top / (LV_ROWS - 1) + 1);
	while (h[n])
		n++;
	h[n++] = '/';
	lv_num(h + n, (nlines + LV_ROWS - 2) / (LV_ROWS - 1));
	lv_text(0, 0, h, 0xff000000u, 0xffffff00u);
	for (uint32_t r = 0; r < LV_ROWS - 1; r++) {
		uint32_t li = top + r;
		char line[LV_COLS + 1];
		uint32_t len = 0;
		if (li < nlines) {
			len = lv_line_len[li];
			for (uint32_t k = 0; k < len; k++)
				line[k] = lv_buf[lv_line_off[li] + k];
		}
		line[len] = 0;
		lv_text(0, r + 1, line, 0xffe0e0e0u, 0xff101018u);
	}
}

void log_view(void)
{
	con_clear(0xff101018u);
	con_str("LOGVIEW", "reading the iOS kernel log (read-only)");
	int rc = ufs_open_quiet();
	if (rc) {
		con_hex("UFS open failed", (uint32_t)-rc);
		return;
	}
	uint64_t first = 0, last = 0;
	if ((rc = find_userdata(&first, &last))) {
		con_hex("no userdata in GPT", (uint32_t)-rc);
		return;
	}
	uint32_t hdr_lba = (uint32_t)(last + 1 - 1025);
	if (read_blocks(hdr_lba, 1, data)) {
		con_str("LOGVIEW", "header read error");
		return;
	}
	static const char magic[] = "SWEETLOG";
	int ok = 1;
	for (int i = 0; i < 8; i++)
		ok &= data[i] == (uint8_t)magic[i];
	if (!ok) {
		con_hex2("userdata", "first=", (uint32_t)first, "last=", (uint32_t)last);
		con_hex2("expected hdr", "lba=", hdr_lba, "bytes0-3=",
			 data[0] | data[1] << 8 | data[2] << 16 | (uint32_t)data[3] << 24);
		/* search the last 4096 blocks of userdata and the first blocks after
		 * the HFS volume (100 GiB) for the header, in case the base differs */
		con_str("LOGVIEW", "searching the end of userdata for SWEETLOG...");
		uint32_t found = 0;
		for (uint32_t b = (uint32_t)last - 4096; b <= (uint32_t)last && !found; b++) {
			if (read_blocks(b, 1, data))
				break;
			int m = 1;
			for (int i = 0; i < 8; i++)
				m &= data[i] == (uint8_t)magic[i];
			if (m)
				found = b;
		}
		con_hex2("search", "found lba=", found, "rel=", found ? found - (uint32_t)first : 0);
		con_hex2("HFS hdr", "blk1 sig=", 0, "", 0);
		if (!read_blocks((uint32_t)first, 1, data))
			con_hex2("HFS hdr", "sig@1024=", data[1024] << 8 | data[1025], "blocks@1064=",
				 (uint32_t)data[1064] << 24 | data[1065] << 16 | data[1066] << 8 | data[1067]);
		if (!found) {
			con_str("LOGVIEW", "no SWEETLOG anywhere near the end: iOS never wrote its log");
			return;
		}
		hdr_lba = found;
		read_blocks(hdr_lba, 1, data);
	}
	uint32_t size = data[12] | data[13] << 8 | data[14] << 16 | (uint32_t)data[15] << 24;
	uint64_t total = le64(data + 16);
	if (size == 0 || size > 1024 * BLOCK || (size % BLOCK)) {
		con_hex("bad ring size", size);
		return;
	}
	/* stream bytes [from, total) with from = total - tail (or 0) */
	uint64_t tail = (uint64_t)LV_TAIL_BLOCKS * BLOCK;
	if (tail > size)
		tail = size;
	if (tail > total)
		tail = total;
	uint64_t from = total - tail;
	uint32_t got = 0;
	while (got < tail) {
		uint64_t pos = (from + got) % size;			/* ring offset */
		uint32_t blk = (uint32_t)(pos / BLOCK), off = (uint32_t)(pos % BLOCK);
		if (read_blocks(hdr_lba + 1 + blk, 1, data)) {
			con_str("LOGVIEW", "ring read error");
			return;
		}
		uint32_t take = BLOCK - off;
		if (take > tail - got)
			take = (uint32_t)(tail - got);
		for (uint32_t k = 0; k < take; k++)
			lv_buf[got + k] = data[off + k];
		got += take;
	}
	/* split into display lines (wrap at LV_COLS) */
	uint32_t nlines = 0, start = 0;
	for (uint32_t i = 0; i <= got && nlines < LV_MAXLINES; i++) {
		if (i == got || lv_buf[i] == '\n' || i - start == LV_COLS) {
			lv_line_off[nlines] = start;
			lv_line_len[nlines] = (uint16_t)(i - start);
			nlines++;
			start = (i < got && lv_buf[i] == '\n') ? i + 1 : i;
		}
	}
	lv_keys_init();
	uint32_t per = LV_ROWS - 1;
	uint32_t top = nlines > per ? nlines - per : 0;
	char hdr[64];
	int n = 0;
	for (const char *s = "iOS KERNEL LOG  VOL+ older  VOL- newer  page"; *s; s++)
		hdr[n++] = *s;
	hdr[n] = 0;
	lv_show(nlines, top, hdr);
	for (;;) {				/* stays until the phone is switched off */
		if (lv_volup() && top > 0) {
			top = top > per ? top - per : 0;
			lv_show(nlines, top, hdr);
			delay_ms(350);
		} else if (lv_voldown() && top + per < nlines) {
			top += per;
			lv_show(nlines, top, hdr);
			delay_ms(350);
		}
		delay_ms(20);
	}
}
