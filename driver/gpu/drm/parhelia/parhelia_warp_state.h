/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PARHELIA_WARP_STATE_H
#define PARHELIA_WARP_STATE_H

#include <linux/types.h>

static inline void parhelia_stage_commands(__u32 out[13], __u32 mode,
		__u32 a, __u32 b, __u32 c, __u32 stage0)
{
	const __u32 commands[] = {
		11, 0x212121dd, 0, 0x33323c3c, mode, mode,
		a, b, 0x30363534, a, b, c, stage0,
	};
	__u32 i;

	for (i = 0; i < 13; i++)
		out[i] = commands[i];
}

/* Working capture: parhelia-warp-programs-decoded.md sections4 and7.
 * These special 0x41 payloads are program inputs, NOT generic constant
 * loads. Keep them kernel-owned; the userspace constant validator correctly
 * rejects their non-(count,first,1,0) subheaders.
 */
static const __u32 parhelia_publish_offsets[] = { 0, 5, 10, 16, 25, 31 };
static inline void parhelia_publish_commands(__u32 out[31], __u32 publish,
		__u32 kind12, __u32 kind6, __u32 stage6)
{
	const __u32 commands[] = {
		3, 0x212c1000, 0, 4, 0x0002305c,
		3, 0x212e2e2d, 0, 0x83c00000, 0,
		4, 0x403c3cdd, 0, 0x20000028, 0x20000028, publish,
		0x41020007, 0x03800015, 0, 0x3f800000, kind12,
		0x3f803f80, kind6, 0xffffffff, stage6,
		4, 0x4c3c3cdd, 0, 0x20000028, 0x20000028, 0x0200300c,
	};
	__u32 i;

	for (i = 0; i < 31; i++)
		out[i] = commands[i];
}

static const __u32 parhelia_setup_offsets[] = { 0, 5, 10, 18, 35, 41, 49, 62, 73 };
static inline void parhelia_setup_commands(__u32 out[73], __u32 base,
		__u32 kind6, __u32 vertex, __u32 a, __u32 b, __u32 c)
{
	const __u32 commands[] = {
		3, 0x212c1000, 0, 4, 0x0002305c,
		3, 0x212e2e2d, 0, 0x83c00000, 0,
		6, 0x3c3cdd4c, 0x0700300c, 0, 0x20000028, 0x20000028, 0x21212140, base,
		0x4106000f, 0xe0, 0x160, 0x80, 0x800,
		0, 1, 2, 3, 0x02003004, 0x0200300c, 0, 0,
		kind6, 0xffffffff, 0xffffffff, 0xffffffff,
		4, 0x4c3c3cdd, 0, 0x20000028, 0x20000028, 0x0200300c,
		6, 0x212121dd, 0, 0x3e403c3c, 0x20000002, 0x20000002, vertex, 2,
		11, 0x212121dd, 0, 0x33323c3c, 0x20000002, 0x20000002,
		a, b, 0x30363534, a, b, c, 0,
		9, 0x212121dd, 0, 0x4c3c3c3c, 0x20000002, 0x20000002,
		2, 0x0200300c, 0x21215147, 0x00000fdb, 0,
	};
	__u32 i;

	for (i = 0; i < 73; i++)
		out[i] = commands[i];
}

/* The five CALLs from p3d_emit_state's constant phase, with the three loads
 * kept in ONE CALL. Shared with the original-source stream-parity test.
 */
static const __u32 parhelia_loader_offsets[] = { 0, 5, 10, 16, 75, 81 };

static inline void parhelia_loader_commands(__u32 out[81], __u32 entry,
		const __u32 constants[28], const __u32 viewport[8])
{
	__u32 i;
	const __u32 preamble[] = {
		3, 0x212c1000, 0, 4, 0x0002305c,
		3, 0x212e2e2d, 0, 0x83c00000, 0,
		4, 0x403c3cdd, 0, 0x20000028, 0x20000028, entry,
	};
	const __u32 tail[] = { 4, 0x4c3c3cdd, 0, 0x20000028, 0x20000028, 0x0200200c };

	for (i = 0; i < 16; i++)
		out[i] = preamble[i];
	out[16] = 0x410e001f;
	out[17] = 7;
	out[18] = 0;
	out[19] = 1;
	out[20] = 0;
	for (i = 0; i < 28; i++)
		out[21 + i] = constants[i];
	for (i = 0; i < 2; i++) {
		__u32 j, pos = 49 + i * 13;

		out[pos] = 0x4104000b;
		out[pos + 1] = 2;
		out[pos + 2] = i ? 0x160 : 8;
		out[pos + 3] = 1;
		out[pos + 4] = 0;
		for (j = 0; j < 8; j++)
			out[pos + 5 + j] = viewport[j];
	}
	for (i = 0; i < 6; i++)
		out[75 + i] = tail[i];
}

#endif
