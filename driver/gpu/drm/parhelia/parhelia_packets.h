/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PARHELIA_PACKETS_H
#define PARHELIA_PACKETS_H

#include <linux/errno.h>
#include <linux/types.h>

/* The canonical captured bind and the reference p3d bind (which also writes
 * 0x1104). Only the supported triangle class selects this auxiliary image.
 * Constant-loader binds must never acquire a geometry-stage packet.
 */
static inline __u32 parhelia_vertex_bind_mode(const __u32 *cmd, __u32 length)
{
	if ((length != 8 && length != 10) || cmd[0] != length - 2 ||
	    cmd[1] != 0x212121dd || cmd[4] != 0x20000002 || cmd[5] != cmd[4])
		return 0;
	if (length == 8)
		return cmd[3] == 0x3e403c3c && cmd[7] == 2 ? cmd[4] : 0;
	return cmd[3] == 0x41403c3c && cmd[6] == cmd[7] &&
		cmd[8] == 0x2121213e && cmd[9] == 2 ? cmd[4] : 0;
}

/* Storage ids are independent of execution order. The first n blocks are
 * userspace packet storage, followed by three opening/EOB/drain blocks per
 * logical section and then separate stage CALLs. packet_spans[] is zero for
 * those synthetic blocks and initially one for each userspace block. A later
 * validated pass may combine adjacent packet-storage blocks into one CALL
 * without changing any packet bytes or hardware header boundaries.
 */
#define PARHELIA_CALL_CAPACITY 224u
struct parhelia_call_plan {
	__u32 blocks[PARHELIA_CALL_CAPACITY];
	__u16 packet_spans[PARHELIA_CALL_CAPACITY];
	__u32 stage_modes[PARHELIA_CALL_CAPACITY];
	__u32 stage_parent[PARHELIA_CALL_CAPACITY];
	__u32 count, stages, sync_sections;
};

static inline void parhelia_plan_append(struct parhelia_call_plan *plan,
		__u32 block, __u16 packet_span)
{
	plan->blocks[plan->count] = block;
	plan->packet_spans[plan->count] = packet_span;
	plan->count++;
}

static inline int parhelia_plan_calls(struct parhelia_call_plan *plan,
		__u32 n, __u32 opening, int full_sync)
{
	__u32 i, stages = 0;

	plan->count = plan->stages = 0;
	plan->sync_sections = 1;
	if (!n || n > PARHELIA_CALL_CAPACITY - 3 || opening > n)
		return -EINVAL;
	for (i = 0; i < n; i++)
		stages += !!plan->stage_modes[i];
	/* Reserve the drain storage even when full_sync embeds it in primary. */
	if (n + 3 + stages > PARHELIA_CALL_CAPACITY)
		return -E2BIG;
	for (i = 0; i < n; i++) {
		if (i == opening)
			parhelia_plan_append(plan, n, 0);
		parhelia_plan_append(plan, i, 1);
		if (plan->stage_modes[i]) {
			parhelia_plan_append(plan, n + 3 + plan->stages, 0);
			plan->stage_parent[plan->stages++] = i;
		}
	}
	if (opening == n)
		parhelia_plan_append(plan, n, 0);
	parhelia_plan_append(plan, n + 1, 0);
	if (!full_sync)
		parhelia_plan_append(plan, n + 2, 0);
	return 0;
}

/* Preserve both logical fence brackets while coalescing one producer update.
 * The initial hardware-validation ABI accepts exactly one boundary, hence two
 * sections.  @opening0 and @opening1 are absolute packet indices in their
 * respective half-open ranges.
 */
static inline int parhelia_plan_calls_stateful(struct parhelia_call_plan *plan,
		__u32 n, __u32 boundary, __u32 opening0, __u32 opening1)
{
	__u32 i, stages = 0;

	plan->count = plan->stages = 0;
	plan->sync_sections = 2;
	if (!n || boundary == 0 || boundary >= n || opening0 >= boundary ||
	    opening1 < boundary || opening1 >= n)
		return -EINVAL;
	for (i = 0; i < n; i++)
		stages += !!plan->stage_modes[i];
	if (n + 6 + stages > PARHELIA_CALL_CAPACITY)
		return -E2BIG;

	for (i = 0; i < boundary; i++) {
		if (i == opening0)
			parhelia_plan_append(plan, n, 0);
		parhelia_plan_append(plan, i, 1);
		if (plan->stage_modes[i]) {
			parhelia_plan_append(plan, n + 6 + plan->stages, 0);
			plan->stage_parent[plan->stages++] = i;
		}
	}
	parhelia_plan_append(plan, n + 1, 0);
	parhelia_plan_append(plan, n + 2, 0);
	for (i = boundary; i < n; i++) {
		if (i == opening1)
			parhelia_plan_append(plan, n + 3, 0);
		parhelia_plan_append(plan, i, 1);
		if (plan->stage_modes[i]) {
			parhelia_plan_append(plan, n + 6 + plan->stages, 0);
			plan->stage_parent[plan->stages++] = i;
		}
	}
	parhelia_plan_append(plan, n + 4, 0);
	parhelia_plan_append(plan, n + 5, 0);
	return 0;
}

static inline int parhelia_is_fence_boundary(const __u32 *cmd, __u32 length)
{
	return length == 3 && cmd[0] == 1 &&
		cmd[1] == 0x212121ac && cmd[2] == 0x3fff8842;
}

/* A CALL may contain the three consecutive constant loads emitted by
 * p3d_emit_constant_group(). Keep the CALL intact, checking every header and
 * sub-header rather than treating its first packet as the entire section.
 * Only the constant-file form is accepted, not bind-table/base-register 0x41.
 */
static inline int parhelia_validate_constant_group(const __u32 *cmd, __u32 length)
{
	__u32 pos = 0;

	if (!length)
		return -EINVAL;
	while (pos < length) {
		__u32 words, vectors;

		if (length - pos < 5)
			return -EINVAL;
		words = (cmd[pos] & 0xffff) + 2;
		vectors = cmd[pos + 1];
		if (words > length - pos || !vectors || vectors > 63 ||
		    words != vectors * 4 + 5 ||
		    cmd[pos] != (0x41000000u | (vectors << 17) | (words - 2)) ||
		    cmd[pos + 3] != 1 || cmd[pos + 4] != 0)
			return -EINVAL;
		pos += words;
	}
	return 0;
}

/* Exact p3d_fence_block()/p3d_emit_draws() packets, with the optional
 * tri_ringreplay --rrfence auto patch. Addresses and counter allocation stay
 * kernel-owned. Shared with the host stream-parity test, not a userspace ABI.
 */
static inline void parhelia_reference_fences(__u32 *start, __u32 *eob,
		__u32 *drain, __u32 begin, __u32 finish, __u32 dwgctl, int replay)
{
	start[0] = 3;
	start[1] = 0x21afafdd;
	start[2] = 0;
	start[3] = begin | 1;
	start[4] = replay ? begin + 8 : begin;
	eob[0] = 1;
	eob[1] = 0x212121ae;
	eob[2] = 0x00070601;
	drain[0] = 4;
	drain[1] = 0xc0afafc0;
	drain[2] = 0x307;
	drain[3] = (replay ? finish - 8 : finish) | 1;
	drain[4] = finish;
	drain[5] = dwgctl;
}

#endif
