// SPDX-License-Identifier: MIT
/*
 * LZBITMAP ("ZBM") decompressor for APFS decmpfs types 13/14, as a small
 * shared library for tools/flame-iosroot.py (ctypes).
 *
 * Port of the decompression logic of libzbitmap by Ernesto A. Fernández
 * (Copyright (C) 2022 Corellium LLC, GPL-2.0+ OR MIT), as carried in
 * linux-apfs-rw/libzbitmap.c. Same checks, same behaviour.
 *
 *   long zbm_decompress(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size)
 *   -> bytes written, or < 0 on error
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ZBM_MAX_DECMP_CHUNK_SIZE 0x8000
#define BITMAP_COUNT 12
#define BITMAP_BASE 3
#define BITMAP_BYTECNT 17
#define MAX_PERIOD_BYTECNT 2

struct bmap { uint8_t bitmap, period_bytecnt; };

struct st {
	uint8_t *dest; size_t dest_left; uint32_t written; uint16_t period;
	const uint8_t *src_end; uint32_t len, decmp_len;
	const uint8_t *src; size_t src_left; size_t prewritten;
	const uint8_t *data, *meta_1, *meta_2;
	const uint8_t *m3_addr; int m3_nib;
	struct bmap bitmaps[BITMAP_COUNT];
};

static uint32_t u24(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }

static int read_nibble(struct st *s, uint8_t *r)
{
	if (s->m3_addr >= s->src_end) return -1;
	if (s->m3_nib == 0) { *r = *s->m3_addr & 0xf; s->m3_nib = 1; }
	else { *r = (*s->m3_addr >> 4) & 0xf; s->m3_nib = 0; ++s->m3_addr; }
	return 0;
}

static void rewind_nibble(struct st *s)
{
	if (s->m3_nib == 0) { s->m3_nib = 1; --s->m3_addr; } else s->m3_nib = 0;
}

static int apply_bitmap(struct st *s, const struct bmap *b)
{
	if (b->period_bytecnt) {
		s->period = 0;
		for (int i = 0; i < b->period_bytecnt; ++i) {
			if (s->meta_1 >= s->src_end) return -1;
			s->period |= (uint16_t)(*s->meta_1 << (i * 8));
			++s->meta_1;
		}
	}
	if (s->period == 0) return -1;
	for (int i = 0; i < 8; ++i) {
		if (s->written == s->decmp_len) break;
		if (s->dest_left == 0) return -2;
		if (b->bitmap & (1 << i)) {
			if (s->data >= s->src_end) return -1;
			*s->dest = *s->data++;
		} else {
			if (s->prewritten + s->written < s->period) return -1;
			*s->dest = *(s->dest - s->period);
		}
		++s->dest; --s->dest_left; ++s->written;
	}
	return 0;
}

static int apply_number(struct st *s, uint8_t n)
{
	if (n == 0xf) return -1;
	if (n > MAX_PERIOD_BYTECNT) return apply_bitmap(s, &s->bitmaps[n - BITMAP_BASE]);
	if (s->meta_2 >= s->src_end) return -1;
	struct bmap next = { *s->meta_2, n };
	++s->meta_2;
	return apply_bitmap(s, &next);
}

static int repetition(struct st *s, uint16_t *rep)
{
	uint8_t nib;
	if (s->decmp_len - s->written <= 8) { *rep = 1; return 0; }
	if (read_nibble(s, &nib)) return -1;
	if (nib != 0xf) { rewind_nibble(s); *rep = 1; return 0; }
	uint16_t total = 4;
	while (nib == 0xf) {
		if (read_nibble(s, &nib)) return -1;
		total += nib;
		if (total < nib) return -1;
	}
	*rep = total;
	return 0;
}

static int read_bitmaps(struct st *s)
{
	if (s->len < BITMAP_BYTECNT) return -1;
	const uint8_t *a = s->src_end - BITMAP_BYTECNT;
	int off = 0;
	for (int i = 0; i < BITMAP_COUNT; ++i) {
		uint8_t bm = 0, pc = 0;
		for (int k = 0; k < 10; ++k) {
			if (a >= s->src_end) return -1;
			int bit = (*a >> off) & 1;
			if (++off == 8) { off = 0; ++a; }
			if (k < 8) bm |= (uint8_t)(bit << k); else pc |= (uint8_t)(bit << (k - 8));
		}
		if (pc > MAX_PERIOD_BYTECNT) return -1;
		s->bitmaps[i].bitmap = bm;
		s->bitmaps[i].period_bytecnt = pc;
	}
	return 0;
}

static int compressed_chunk(struct st *s)
{
	s->written = 0;
	s->period = 8;
	if (s->len < 15) return -1;
	s->data = s->src + 15;
	uint32_t o1 = u24(s->src + 6), o2 = u24(s->src + 9), o3 = u24(s->src + 12);
	if (o1 >= s->len || o2 >= s->len || o3 >= s->len) return -1;
	s->meta_1 = s->src + o1;
	s->meta_2 = s->src + o2;
	s->m3_addr = s->src + o3;
	s->m3_nib = 0;
	if (read_bitmaps(s)) return -1;
	while (s->written < s->decmp_len) {
		uint8_t n; uint16_t rep;
		if (read_nibble(s, &n)) return -1;
		if (repetition(s, &rep)) return -1;
		for (int i = 0; i < rep; ++i) {
			int e = apply_number(s, n);
			if (e) return e;
		}
	}
	return 0;
}

long zbm_decompress(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size)
{
	struct st s;
	memset(&s, 0, sizeof(s));
	s.src = src; s.src_left = src_size; s.dest = dst; s.dest_left = dst_size;
	if (src_size < 4 || memcmp(src, "ZBM\x09", 4)) return -10;
	s.src += 4; s.src_left -= 4;
	do {
		if (s.src_left < 6) return -11;
		s.len = u24(s.src);
		if (s.len > s.src_left || s.len < 6) return -12;
		s.src_end = s.src + s.len;
		s.decmp_len = u24(s.src + 3);
		if (s.decmp_len > ZBM_MAX_DECMP_CHUNK_SIZE) return -13;
		if (s.decmp_len > s.dest_left) return -14;
		if (s.len == s.decmp_len + 6) {           /* stored chunk */
			memcpy(s.dest, s.src + 6, s.decmp_len);
			s.dest += s.decmp_len; s.dest_left -= s.decmp_len;
		} else if (s.decmp_len) {
			int e = compressed_chunk(&s);
			if (e) return e * 100;
		}
		s.src += s.len; s.src_left -= s.len;
		s.prewritten += s.decmp_len;
	} while (s.decmp_len != 0);
	return (long)s.prewritten;
}
