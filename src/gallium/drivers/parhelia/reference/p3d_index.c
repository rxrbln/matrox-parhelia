/*
 * parhelia3d - indexed draws.
 *
 * There is no index buffer in the command ring.  Every draw packet the vendor
 * emits is opcode 0x97 with a start and end BYTE ADDRESS of consecutive
 * vertices, and that holds even for the primitives its ICD decomposes into
 * index lists: the quads, quadstrip, polygon and lineloop captures under
 * captures/gl-matrix contain 0x97 range draws and nothing else, at 1, 2, 3 and
 * 4 vertices per packet.  The index list exists only in the ICD's own buffer,
 * where GL_LINE_LOOP's closing segment is a 0x07 packed 16-bit pair; by the
 * time the geometry reaches hardware the indices have been resolved.
 *
 * glDrawElements is therefore served by expanding the indices into a linear
 * vertex stream.  It costs one copy of the drawn vertices, which is what the
 * vendor pays as well.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#include <string.h>

#include "parhelia3d.h"

/* Read one index without assuming the client's array is aligned. */
static size_t
index_at(const void *indices, unsigned index_bytes, size_t i)
{
	const unsigned char *p = (const unsigned char *)indices + i * index_bytes;

	switch (index_bytes) {
	case 1: return p[0];
	case 2: return (size_t)p[0] | ((size_t)p[1] << 8);
	default: return (size_t)p[0] | ((size_t)p[1] << 8)
		      | ((size_t)p[2] << 16) | ((size_t)p[3] << 24);
	}
}

size_t
p3d_deindex(uint32_t *out, size_t out_cap, const uint32_t *verts, size_t nvert,
	    unsigned dw_per_vertex, const void *indices, unsigned index_bytes,
	    size_t count)
{
	size_t need, i;

	if (!out || !verts || !indices || !dw_per_vertex)
		return 0;
	if (index_bytes != 1 && index_bytes != 2 && index_bytes != 4)
		return 0;
	/* Check each multiplication before forming an address from it. */
	if (count > (size_t)-1 / index_bytes ||
	    nvert > (size_t)-1 / dw_per_vertex ||
	    count > out_cap / dw_per_vertex)
		return 0;
	need = count * dw_per_vertex;

	/* Validate first: an error must not leave a partial vertex stream for the
	 * caller to accidentally submit. */
	for (i = 0; i < count; i++) {
		size_t v = index_at(indices, index_bytes, i);

		/* An out-of-range index would fetch whatever follows the vertex
		 * buffer and draw it, so refuse the whole call instead. */
		if (v >= nvert)
			return 0;
	}
	for (i = 0; i < count; i++) {
		size_t v = index_at(indices, index_bytes, i);

		memcpy(out + i * dw_per_vertex, verts + v * dw_per_vertex,
		       dw_per_vertex * sizeof *out);
	}
	return need;
}
