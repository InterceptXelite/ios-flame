/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

/*
 * flame: the vendor DTB carries a placeholder /memory that XBL/ABL patches with
 * the real banks. Find the bank that contains `addr`; returns its base/size.
 * Handles #address-cells/#size-cells of the root (1 or 2 each).
 */
int fdt_memory_bank(uintptr_t fdt, uint64_t addr, uint64_t *base, uint64_t *size)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0, is_mem = 0;
	uint32_t ac = 2, sc = 1;
	uintptr_t reg = 0;
	uint32_t reg_len = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			depth++;
			if (depth == 2) {
				is_mem = 0;
				reg = 0;
				reg_len = 0;
			}
			p = align4(skip_str(p));
			break;
		case FDT_END_NODE:
			if (depth == 2 && is_mem && reg) {
				uint32_t step = (ac + sc) * 4;
				for (uint32_t o = 0; step && o + step <= reg_len; o += step) {
					uint64_t b = ac == 2 ? (uint64_t)be32(reg + o) << 32 | be32(reg + o + 4) : be32(reg + o);
					uintptr_t s_ = reg + o + ac * 4;
					uint64_t s = sc == 2 ? (uint64_t)be32(s_) << 32 | be32(s_ + 4) : be32(s_);
					if (s && addr >= b && addr < b + s) {
						*base = b;
						*size = s;
						return 1;
					}
				}
			}
			depth--;
			if (depth < 0)
				return 0;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			const char *n = (const char *)(strings + nameoff);
			if (depth == 1 && len == 4 && streq(n, "#address-cells"))
				ac = be32(val);
			else if (depth == 1 && len == 4 && streq(n, "#size-cells"))
				sc = be32(val);
			else if (depth == 2 && streq(n, "device_type") && streq((const char *)val, "memory"))
				is_mem = 1;
			else if (depth == 2 && streq(n, "reg")) {
				reg = val;
				reg_len = len;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return 0;
		}
	}
}
