/*
 * parhelia3d - command stream construction.
 *
 * Packet framing, verified on hardware and recorded in
 * docs/parhelia-engine-context-bringup.md:
 *
 *   header low16 = total dwords - 2
 *   opcode 0x00  register list: [hdr][tag][data..], four register indices per
 *                tag word packed LSB-first, index 0x21 terminates a tag and
 *                0x20 is a pad slot; reg = 0x1000 + index * 4
 *   opcode 0x41  block load: [hdr][n][first][1][0][n*4 dwords], the header
 *                carrying n<<17 and n*4+3
 *   opcode 0x97  draw: [hdr][start][end], both plain byte addresses, with
 *                bits [22:17] = dwords per vertex - 1
 *
 * The primary ring carries one secondary CALL per packet - the cadence the
 * hardware accepts - as the five dwords [0x20292820][0][start][end][pad].
 * The call's start tag selects the section framing (|2 header-ful) and the
 * end always carries |3.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#include <string.h>

#include "parhelia3d.h"

#define P3D_CALL_HDR	0x20292820u
#define P3D_CALL_DWORDS	5

void
p3d_cs_init(struct p3d_cs *cs, uint32_t *sec, size_t sec_cap,
	    uint32_t *ring, size_t ring_cap,
	    struct p3d_reloc *rel, size_t rel_cap)
{
	memset(cs, 0, sizeof *cs);
	cs->sec = sec;   cs->sec_cap = sec_cap;
	cs->ring = ring; cs->ring_cap = ring_cap;
	cs->rel = rel;   cs->rel_cap = rel_cap;
}

/* Reserve n dwords in the section blob and open a ring CALL for them.  The
 * call's operands stay zero here: they are bus addresses, which only the
 * submitter knows, so it patches them through the reloc list. */
static uint32_t *
cs_packet(struct p3d_cs *cs, size_t n, int is_draw)
{
	uint32_t *p;
	struct p3d_reloc *r;

	if (cs->error)
		return NULL;
	if (cs->sec_len + n > cs->sec_cap ||
	    cs->ring_len + P3D_CALL_DWORDS > cs->ring_cap ||
	    cs->rel_len + 1 > cs->rel_cap) {
		cs->error = 1;
		return NULL;
	}
	p = cs->sec + cs->sec_len;

	r = &cs->rel[cs->rel_len++];
	r->ring_off = (uint32_t)cs->ring_len;
	r->sec_off  = (uint32_t)cs->sec_len;
	r->sec_len  = (uint32_t)n;
	r->is_draw  = is_draw;

	cs->ring[cs->ring_len + 0] = P3D_CALL_HDR;
	cs->ring[cs->ring_len + 1] = 0;
	/* a recognisable placeholder, so an unrelocated call is obvious in a
	 * dump rather than looking like a valid address of zero */
	cs->ring[cs->ring_len + 2] = 0xdeadbeefu;	/* section start */
	cs->ring[cs->ring_len + 3] = 0xdeadbeefu;	/* section end */
	cs->ring[cs->ring_len + 4] = 0;
	cs->ring_len += P3D_CALL_DWORDS;

	cs->sec_len += n;
	return p;
}

/* Emit pre-built dwords as ONE ring call.  The vendor's constant loads all sit
 * in a single section, so a group that must not be split across calls is built
 * in a scratch buffer and handed over whole. */
void
p3d_raw(struct p3d_cs *cs, const uint32_t *dw, size_t n)
{
	uint32_t *p = cs_packet(cs, n, 0);
	size_t i;

	if (!p)
		return;
	for (i = 0; i < n; i++)
		p[i] = dw[i];
}

/* How many dwords a register list of n writes occupies, honouring breaks. */
static size_t
reglist_size(const struct p3d_rw *rw, size_t n)
{
	size_t i = 0, dw = 1;			/* the header */

	while (i < n) {
		size_t grp = 0;

		if (rw[i].reg == P3D_BREAK) { i++; continue; }
		while (i < n && grp < 4 && rw[i].reg != P3D_BREAK) { grp++; i++; }
		if (i < n && rw[i].reg == P3D_BREAK) i++;
		dw += 1 + grp;			/* tag word plus its data */
	}
	return dw;
}

void
p3d_reglist(struct p3d_cs *cs, const struct p3d_rw *rw, size_t n)
{
	size_t total = reglist_size(rw, n), i = 0, w = 1;
	uint32_t *p = cs_packet(cs, total, 0);

	if (!p)
		return;
	while (i < n) {
		const struct p3d_rw *grp[4];
		size_t g = 0, j;
		uint32_t tag = 0;

		if (rw[i].reg == P3D_BREAK) { i++; continue; }
		while (i < n && g < 4 && rw[i].reg != P3D_BREAK)
			grp[g++] = &rw[i++];
		if (i < n && rw[i].reg == P3D_BREAK)
			i++;

		for (j = 0; j < 4; j++) {
			uint32_t idx = (j < g)
				     ? ((grp[j]->reg - P3D_REG_BASE) / 4) & 0xff
				     : 0x21;	/* 0x21 terminates the tag */
			tag |= idx << (8 * j);
		}
		p[w++] = tag;
		for (j = 0; j < g; j++)
			p[w++] = grp[j]->val;
	}
	p[0] = (uint32_t)(w - 2);		/* low16 = total dwords - 2 */
}

void
p3d_blockload(struct p3d_cs *cs, uint32_t first, const uint32_t *vec4s,
	      size_t nvec)
{
	size_t total = 5 + nvec * 4, i;
	uint32_t *p = cs_packet(cs, total, 0);

	if (!p)
		return;
	p[0] = 0x41000000u | (((uint32_t)nvec << 17) & 0x007e0000u)
			   | (((uint32_t)nvec * 4 + 3) & 0xffffu);
	p[1] = (uint32_t)nvec;
	p[2] = first;
	p[3] = 1;
	p[4] = 0;
	for (i = 0; i < nvec * 4; i++)
		p[5 + i] = vec4s[i];
}

void
p3d_drawpacket(struct p3d_cs *cs, unsigned dw_per_vertex)
{
	uint32_t *p = cs_packet(cs, 3, 1);

	if (!p)
		return;
	/* 0x97 is 0x17 with bit 31 set - the GL path's address-mode draw */
	p[0] = 0x97000000u | (((dw_per_vertex - 1) & 0x3f) << 17) | 0x0001u;
	p[1] = 0;	/* vertex range start, patched at submit */
	p[2] = 0;	/* end */
}

void
p3d_rangepacket(struct p3d_cs *cs, uint32_t first, uint32_t last)
{
	uint32_t *p = cs_packet(cs, 3, 1);

	if (!p)
		return;
	p[0] = 0x8f000001u;
	p[1] = first;
	p[2] = last;
}

void
p3d_rangepacket2(struct p3d_cs *cs, uint32_t first0, uint32_t last0,
		 uint32_t first1, uint32_t last1)
{
	uint32_t *p = cs_packet(cs, 5, 1);

	if (!p)
		return;
	p[0] = 0x8f000003u;
	p[1] = first0;
	p[2] = last0;
	p[3] = first1;
	p[4] = last1;
}

void
p3d_indexpacket_u32(struct p3d_cs *cs)
{
	uint32_t *p = cs_packet(cs, 3, 1);

	if (!p)
		return;
	/* Mode 7, address-mode u32 index list.  0xdf000001 requires a separately
	 * validated layout bit and is deliberately not exposed by this first ABI. */
	p[0] = 0x9f000001u;
	p[1] = 0;
	p[2] = 0;
}

/* Mode 6, address-mode u16 index list; operands as for mode 7. */
void
p3d_indexpacket_u16(struct p3d_cs *cs)
{
	uint32_t *p = cs_packet(cs, 3, 1);

	if (!p)
		return;
	p[0] = 0x9b000001u;
	p[1] = 0;
	p[2] = 0;
}

void
p3d_bindpacket(struct p3d_cs *cs, uint32_t mode, const struct p3d_rw *rw,
	       size_t n, int strobe_alone)
{
	struct p3d_rw buf[16];
	size_t k = 0, i;

	/* the vendor's canonical bind: strobe, the mode latch twice, then the
	 * entries.  With strobe_alone the strobe gets a tag word to itself,
	 * which is what the per-vertex bind does and a four-at-a-time packer
	 * cannot otherwise reproduce. */
	buf[k].reg = P3D_R_STROBE; buf[k].val = 0; k++;
	if (strobe_alone) { buf[k].reg = P3D_BREAK; buf[k].val = 0; k++; }
	buf[k].reg = P3D_R_WARP_MODE; buf[k].val = mode; k++;
	buf[k].reg = P3D_R_WARP_MODE; buf[k].val = mode; k++;
	for (i = 0; i < n && k < sizeof buf / sizeof buf[0]; i++)
		buf[k++] = rw[i];
	p3d_reglist(cs, buf, k);
}
