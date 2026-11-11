/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

static void show_bank(const char *name, uint64_t b, uint64_t sz)
{
	(void)name;
	con_hex2("DRAM", "base_hi=", (uint32_t)(b >> 32), "lo=", (uint32_t)b);
	con_hex2("", "  MiB=", (uint32_t)(sz >> 20), "", 0);
}

static int adt_name_is(const volatile uint8_t *n, const char *want)
{
	int i = 0;
	for (; want[i]; i++)
		if (n[i] != (uint8_t)want[i])
			return 0;
	return n[i] == 0;
}

static void adt_set_u64(uintptr_t adt, uint64_t len, const char *name, uint64_t v)
{
	for (uint64_t o = 0; o + 44 <= len; o += 4) {
		const volatile uint8_t *n = (const volatile uint8_t *)(adt + o);
		if (n[0] != (uint8_t)name[0] || !adt_name_is(n, name))
			continue;
		volatile uint32_t *lenp = (volatile uint32_t *)(adt + o + 32);
		if ((*lenp & 0x7fffffff) != 8)
			continue;
		volatile uint32_t *val = (volatile uint32_t *)(adt + o + 36);
		val[0] = (uint32_t)v;
		val[1] = (uint32_t)(v >> 32);
		return;
	}
	con_str("ADT", "property not found (kernel uses boot_args)");
}

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);

	/* flame boots a v4 boot image: ABL hands over vendor_boot's ramdisk(s)
	 * concatenated IN FRONT of ours, so search for the payload magic. The build
	 * pads our ramdisk (tools/flame-mkboot.py) so the payload lands 4K-aligned;
	 * the scan still walks 8-byte steps in case ABL packs the ramdisks differently. */
	const struct payload *p = 0;
	/* flame: ABL puts our ramdisk directly behind vendor_boot's, at whatever
	 * byte offset that ends on, so scan BYTE by byte (MMU off = Device memory:
	 * only byte loads are safe at an unknown alignment). */
	const volatile uint8_t *b = (const volatile uint8_t *)rd_start;
	uint64_t n = rd_end - rd_start, at = n;
	static const char want[] = "SWEETXNU";
	for (uint64_t i = 0; i + sizeof(struct payload) <= n; i++) {
		if (b[i] != 'S')
			continue;
		int k = 1;
		while (k < 8 && b[i + k] == (uint8_t)want[k])
			k++;
		if (k == 8) {
			at = i;
			break;
		}
	}
	if (at == n)
		return "no SWEETXNU payload";
	uint64_t src = rd_start + at, dst = src & ~7UL;
	con_hex2("PAYLOAD", "at=", src, "skip=", at);
	if (dst != src) {
		/* Slide everything from the payload to the end of the ramdisk down
		 * to an 8-byte boundary (< 8 bytes, forward byte copy is safe). */
		volatile uint8_t *d = (volatile uint8_t *)dst;
		for (uint64_t i = 0; i < rd_end - src; i++)
			d[i] = b[at + i];
		rd_end -= src - dst;
		con_hex2("PAYLOAD", "moved to=", dst, "by=", src - dst);
	}
	p = (const struct payload *)dst;
	rd_start = dst;
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";

	/* flame: 4 GB and 6 GB variants exist and the upper DRAM is split into
	 * banks by carve-outs; ABL patches /memory at boot. If the build-time base
	 * is not inside a bank, take the largest bank above 4 GiB instead
	 * (32 MiB aligned: XNU maps the kernel with 32 MiB blocks), and refuse if
	 * there is none. Every PA below is derived from `phys`. */
	uint64_t phys = p->phys_base, mem_size = p->mem_size, bank_b, bank_s;
	if (!fdt_memory_bank(dtb, phys, &bank_b, &bank_s)) {
		fdt_memory_dump(dtb, show_bank);
		if (!fdt_memory_largest(dtb, 0x100000000UL, &bank_b, &bank_s))
			return "no DRAM bank above 4G in ABL's /memory";
		phys = (bank_b + 0x1ffffffUL) & ~0x1ffffffUL;
		if (phys >= bank_b + bank_s)
			return "DRAM bank too small";
		con_hex("RELOC phys", phys);
	}
	if (phys + mem_size > bank_b + bank_s)
		mem_size = (bank_b + bank_s - phys) & ~(PAGE_16K - 1);
	if (mem_size < 0x30000000UL)
		return "DRAM bank < 768 MiB for XNU";
	uint64_t pa_of_va_base = phys - p->virt_base;	/* pa = va + this */
	con_hex("PHYS base", phys);
	con_hex2("PHYS", "MiB=", mem_size >> 20, "bank MiB=", bank_s >> 20);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > phys && rd_start < phys + mem_size)
		return "ramdisk overlaps kernel memory";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	/* flame: keep the ADT's /chosen dram-base/dram-size equal to what we hand
	 * the kernel (phys may have moved at run time). ADT property: name[32],
	 * u32 length, value; values are only 4-byte aligned -> two 32-bit stores. */
	adt_set_u64(adt_pa, p->adt_size, "dram-base", phys);
	adt_set_u64(adt_pa, p->adt_size, "dram-size", mem_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = phys;
	ba->memSize = mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
