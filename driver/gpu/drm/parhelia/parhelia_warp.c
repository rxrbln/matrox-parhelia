// SPDX-License-Identifier: GPL-2.0-only
/* Fixed-function WARP programs recovered and assembled in parhelia3d.
 * Copyright 2026 René Rebe <rene@exactco.de>
 * The kernel owns the instruction store so a new DRM context works without
 * relying on microcode left behind by a vendor driver or userspace MMIO.
 */
#include <linux/delay.h>
#include <linux/crc32.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#include "parhelia_drv.h"
#include "parhelia_firmware.h"
#include "parhelia_warp_state.h"

#define WARP_FW_MODULE(name, start, count, crc) MODULE_FIRMWARE(name);
PARHELIA_RESIDENT_FIRMWARE(WARP_FW_MODULE)
#undef WARP_FW_MODULE

static const struct {
	const char *name;
	u32 start, count, crc;
} warp_residents[] = {
#define WARP_FW_DESC(name, start, count, crc) { name, start, count, crc },
	PARHELIA_RESIDENT_FIRMWARE(WARP_FW_DESC)
#undef WARP_FW_DESC
};

static bool warp_probe;
module_param(warp_probe, bool, 0400);
MODULE_PARM_DESC(warp_probe, "Run kernel-owned constant-store WARP probe before exposing DRM");

static bool vertex_probe;
module_param(vertex_probe, bool, 0400);
MODULE_PARM_DESC(vertex_probe, "Record fetched vertex attributes in c15/c14 (requires warp_probe)");

/* Assembled from warp/constant-probe.wasm, not extracted vendor object code. */
static const u32 warp_constant_probe[] = {
	0x000803ff, 0x00440200, 0xb01a21a2,
	0x00000200, 0x00040000, 0x14c00000,
	0x00080200, 0x00440000, 0x301a21a2,
};

/* Assembled from warp/constant-loader.wasm using the documented scalar and
 * dual-index fields. The program preserves a0 and uses r6/scalar s7 scratch.
 */
static const u32 warp_constant_loader[] = {
	0x0008016f, 0x30443000, 0x301a21a2,
	0x00700600, 0x00040000, 0x19800000,
	0x00080011, 0x404f1200, 0x305a2092,
	0x00080012, 0x804f1200, 0x305a2124,
	0x00740200, 0x0044001c, 0x21000000,
	0x00018200, 0x01440000, 0x11c00000,
	0x00000200, 0x00040000, 0x00000000,
	0x0008030f, 0x019d2200, 0x301a21a2,
	0x00180013, 0x20443000, 0x301a21a2,
	0x00000200, 0xfe840000, 0x11ffffff,
	0x00000200, 0x00040000, 0x00000000,
	0x0268001f, 0x004f1200, 0x301a21a2,
	0x00000200, 0x00040000, 0x14c00000,
	0x00000200, 0x00040000, 0x00000000,
};

/* The passthrough and multitexture programs write temporary T2 (mul T2,
 * v5, r1.0000 | mov o11, v0; add o1, v5, r1.0000) as the replace program
 * does: draws depend on T2, which after power-on holds garbage (every draw
 * rejected) and after a warm reboot a stale value (engine lock-up) until
 * some program writes it. */
static const u32 warp_passthrough[] = {
	0x00083e00, 0x00440000, 0xb89a2249,
	0x0800bebf, 0x00440215, 0xba9a2249,
	0x08087e00, 0x00440015, 0xb89a2249,
	0x00000200, 0x00040000, 0x14c00000,
	0x00080200, 0x00440000, 0x301a21a2,
};

/* Assembled from warp/vertex-probe.wasm. Six instructions fit in the
 * passthrough allocation; ordinary rendering keeps the four-
 * instruction reference program above unchanged.
 */
static const u32 warp_vertex_probe[] = {
	0x000803ff, 0x00440200, 0xb01a21a2,
	0x005803ef, 0x00440200, 0xb01a21a2,
	0x00083e00, 0x00440000, 0xb89a2249,
	0x08087ebf, 0x00440215, 0xb89a2249,
	0x00000200, 0x00040000, 0x14c00000,
	0x00080200, 0x00440000, 0x301a21a2,
};

/* No T2 write here: in this program it locked up vertex-alpha draws.  The
 * passthrough and multitexture programs initialize T2 instead. */
static const u32 warp_textured[] = {
	0x00083e00, 0x00440000, 0xb89a2249,
	0x08087ebf, 0x00440215, 0xb89a2249,
	0x0078023f, 0x00440200, 0xb01a21a2,
	0x00000200, 0x00040000, 0x14c00000,
	0x00080200, 0x00440000, 0x301a21a2,
};

/* Two-coordinate-set version of warp_textured.  The vertex fetch maps the
 * second set to v8 and 0x1118 maps it to o4.  Writing o4 is required: the
 * hardware otherwise removes texcoord-set 1 from 0x1110 before the draw.
 */
static const u32 warp_textured_multitex[] = {
	0x00083e00, 0x00440000, 0xb89a2249,
	0x0800bebf, 0x00440215, 0xba9a2249,
	0x08087e00, 0x00440015, 0xb89a2249,
	0x0078023f, 0x00440200, 0xb01a21a2,
	0x0088024f, 0x00440200, 0xb01a21a2,
	0x00000200, 0x00040000, 0x14c00000,
	0x00080200, 0x00440000, 0x301a21a2,
};

static const u32 warp_transform[] = {
	0x00003e00, 0x00440000, 0xb89a2249,
	0x1804a200, 0x00440103, 0xbb9a21a2,
	0x10049200, 0x00440103, 0xbb9a21a2,
	0x08048a00, 0x00440103, 0xbb9a21a2,
	0x00048600, 0x00440103, 0xbb9a21a2,
	0x0e987c1f, 0x00440215, 0xb89a2249,
	0x12257ebf, 0x00440289, 0xc49a22a2,
	0x2c105e00, 0x004401a1, 0xbb6a2222,
	0x08083e00, 0x00440105, 0xb89a2249,
	0x00000200, 0x00040000, 0x14c00000,
	0x00080200, 0x00440000, 0x301a21a2,
};

/* Assembled from warp/lighting.wasm: reference lighting plus c6 ambient. */
static const u32 warp_lighting[] = {
	0x00003e00, 0x00440000, 0xb89a2249,
	0x1804a200, 0x00440103, 0xbb9a21a2,
	0x10049200, 0x00440103, 0xbb9a21a2,
	0x08048a00, 0x00440103, 0xbb9a21a2,
	0x00048600, 0x00440103, 0xbb9a21a2,
	0x0e987c1f, 0x00440215, 0xb89a2249,
	0x2004fe00, 0x00440017, 0xbc1a21a2,
	0x0804fe00, 0x0044008d, 0xb11a2249,
	0x1a10be00, 0x00440195, 0xbb5a2249,
	0x0c20be00, 0x40440199, 0xbb1a22db,
	0x08087e00, 0x00440109, 0xb89a2249,
	0x12257ebf, 0x00440289, 0xc49a22a2,
	0x2c105e00, 0x004401a1, 0xbb6a2222,
	0x08083e00, 0x00440105, 0xb89a2249,
	0x00000200, 0x00040000, 0x14c00000,
	0x00080200, 0x00440000, 0x301a21a2,
};

static const u32 warp_replace[] = {
	0x00083e00, 0x00440000, 0xb89a2249,
	0x0800bebf, 0x00440215, 0xba9a2249,
	0x08087e00, 0x40440109, 0xb89a22db,
	0x0078023f, 0x00440200, 0xb01a21a2,
	0x00000200, 0x00040000, 0x14c00000,
	0x00080200, 0x00440000, 0x301a21a2,
};

static int warp_gate_ready(struct parhelia_device *pdev)
{
	unsigned int i;

	for (i = 0; i < 200000; i++) {
		u32 bm = parhelia_read(pdev, 0x08f0);
		u32 a = parhelia_read(pdev, 0x0908);
		u32 b = parhelia_read(pdev, 0x0924);

		/* Bring-up section 1.9: do not overwrite a busy instruction store.
		 * No driver submission exists yet, so there is no 0x450 tag to wait for.
		 */
		if ((bm & 0x03020000) == 0x00020000 &&
		    (a & 0xfff7ffbf) == 0x63a78001 && (b & 0xfff) == 0x100)
			return 0;
		udelay(1);
	}
	dev_err(&pdev->pdev->dev,
		"WARP store busy: BM=%08x 908=%08x 924=%08x\n",
		parhelia_read(pdev, 0x08f0), parhelia_read(pdev, 0x0908),
		parhelia_read(pdev, 0x0924));
	return -ETIMEDOUT;
}

static void warp_upload(struct parhelia_device *pdev, unsigned int instruction,
			const u32 *code, size_t dwords)
{
	size_t i;

	for (i = 0; i < dwords; i++)
		put_unaligned_le32(code[i], pdev->warp_cpu + instruction * 12 + i * 4);
}

static int warp_load_residents(struct parhelia_device *pdev)
{
	const struct firmware *images[ARRAY_SIZE(warp_residents)] = {};
	u32 i, end = 0;
	int ret;

	/* Fetch and validate the ENTIRE set before changing instruction memory.
	 * Missing/wrong-revision firmware is a probe error, never permission to
	 * expose a device dependent on whatever an earlier driver left behind.
	 */
	for (i = 0; i < ARRAY_SIZE(images); i++) {
		if (warp_residents[i].start < end ||
		    warp_residents[i].start + warp_residents[i].count > PARHELIA_WARP_RESIDENT_END) {
			ret = -EINVAL;
			goto out;
		}
		end = warp_residents[i].start + warp_residents[i].count;
		ret = request_firmware(&images[i], warp_residents[i].name,
				       &pdev->pdev->dev);
		if (ret)
			goto out;
		if (images[i]->size != warp_residents[i].count * 12 ||
		    (crc32_le(~0u, images[i]->data, images[i]->size) ^ ~0u) !=
		    warp_residents[i].crc) {
			dev_err(&pdev->pdev->dev, "Invalid resident firmware %s (size/revision)\n",
				warp_residents[i].name);
			ret = -EINVAL;
			goto out;
		}
	}
	ret = warp_gate_ready(pdev);
	if (ret)
		goto out;
	for (i = 0; i < ARRAY_SIZE(images); i++) {
		memcpy(pdev->warp_cpu + warp_residents[i].start * 12,
		       images[i]->data, images[i]->size);
	}
	dev_info(&pdev->pdev->dev,
		 "WARP firmware staged: 9 complete resident/setup images in owned DMA heap\n");
out:
	for (i = 0; i < ARRAY_SIZE(images); i++)
		release_firmware(images[i]);
	return ret;
}

static void warp_read_constant(struct parhelia_device *pdev, u32 slot, u32 out[4])
{
	u32 comp, settle;

	/* Current warp_regread(), not the superseded four-reads-per-vec summary:
	 * each component has its own address, and needs FOUR settling reads.
	 */
	for (comp = 0; comp < 4; comp++) {
		parhelia_write(pdev, 0x0910, (3u << 24) | (comp << 16) | slot);
		parhelia_write(pdev, 0x0938, 0xfffffffcu);
		for (settle = 0; settle < 4; settle++)
			out[comp] = parhelia_read(pdev, 0x0914);
	}
}

void parhelia_warp_trace_vertex(struct parhelia_device *pdev, const char *phase)
{
	u32 position[4], colour[4], bank;

	if (!vertex_probe)
		return;
	/* Only the calibrated kind3 constant port; no scans of undefined spaces.
	 * On timeout these are diagnostic observations, not a completion fence.
	 */
	for (bank = 0; bank < 4; bank++) {
		warp_read_constant(pdev, (bank << 9) | 15, position);
		warp_read_constant(pdev, (bank << 9) | 14, colour);
		dev_info(&pdev->pdev->dev,
			 "vertex probe %s bank%u: c15=%08x %08x %08x %08x c14=%08x %08x %08x %08x\n",
			 phase, bank, position[0], position[1], position[2], position[3],
			 colour[0], colour[1], colour[2], colour[3]);
	}
}

static int warp_run_commands(struct parhelia_device *pdev, const u32 *commands,
			     const u32 *offsets, u32 calls)
{
	struct parhelia_dma_block *block = &pdev->dma_blocks[0];
	u32 *primary = pdev->primary_cpu;
	u32 tag = ++pdev->next_dma_tag;
	u32 i, waited;

	if (!block->cpu) {
		block->cpu = dmam_alloc_coherent(&pdev->pdev->dev,
			PARHELIA_DMA_BLOCK_DWORDS * sizeof(u32), &block->bus, GFP_KERNEL);
		if (!block->cpu)
			return -ENOMEM;
	}
	memcpy(block->cpu, commands, offsets[calls] * sizeof(u32));
	for (i = 0; i < calls; i++) {
		primary[i * 5] = 0x20292820;
		primary[i * 5 + 1] = 0;
		primary[i * 5 + 2] = lower_32_bits(block->bus + offsets[i] * 4) | 2;
		primary[i * 5 + 3] = lower_32_bits(block->bus + offsets[i + 1] * 4) | 3;
		primary[i * 5 + 4] = 0;
	}
	primary[calls * 5] = 0x21212425;
	primary[calls * 5 + 1] = tag;
	primary[calls * 5 + 2] = lower_32_bits(pdev->writeback_bus) + 0x20;
	dma_wmb();
	parhelia_write(pdev, 0x0404, lower_32_bits(pdev->primary_bus));
	for (i = 0; i <= calls; i++) {
		u32 words = i < calls ? (i + 1) * 5 : calls * 5 + 3;

		parhelia_write(pdev, 0x0408,
			lower_32_bits(pdev->primary_bus + words * 4) | 3);
		for (waited = 0; waited < 50000; waited++) {
			if (parhelia_read(pdev, 0x08f0) & BIT(17))
				break;
			udelay(1);
		}
		if (waited == 50000)
			goto timeout;
	}
	parhelia_write(pdev, 0x0314, 1);
	for (waited = 0; waited < 50000; waited++) {
		if (READ_ONCE(((u32 *)pdev->writeback_cpu)[8]) == tag)
			break;
		udelay(1);
	}
	if (waited == 50000)
		goto timeout;
	dma_rmb();
	return warp_gate_ready(pdev);

timeout:
	dev_err(&pdev->pdev->dev,
		"WARP initialization stalled at packet %u: BM=%08x 908=%08x 924=%08x writeback=%08x want=%08x\n",
		i, parhelia_read(pdev, 0x08f0), parhelia_read(pdev, 0x0908),
		parhelia_read(pdev, 0x0924), READ_ONCE(((u32 *)pdev->writeback_cpu)[8]), tag);
	return -ETIMEDOUT;
}

static int warp_probe_once(struct parhelia_device *pdev, const u32 input[4])
{
	u32 commands[] = {
		3, 0x212c1000, 0, 4, 0x0002305c,
		3, 0x212e2e2d, 0, 0x83c00000, 0,
		6, 0x41403c3c, 0x20000028, 0x20000028,
		   parhelia_warp_entry(pdev, PARHELIA_WARP_PROBE_ENTRY),
		   parhelia_warp_entry(pdev, PARHELIA_WARP_PROBE_ENTRY),
		   0x2121213c, 0x20000028,
		0x41000003, input[0], input[1], input[2], input[3],
	};
	static const u32 offsets[] = { 0, 5, 10, 18, 23 };
	u32 result[4];
	int ret = warp_run_commands(pdev, commands, offsets, 4);

	if (ret)
		return ret;
	warp_read_constant(pdev, 15, result);
	dev_info(&pdev->pdev->dev,
		"WARP probe c15=%08x %08x %08x %08x, input=%08x %08x %08x %08x\n",
		result[0], result[1], result[2], result[3],
		input[0], input[1], input[2], input[3]);
	return memcmp(result, input, sizeof(result)) ? -EIO : 0;
}

static int warp_initialize_constants(struct parhelia_device *pdev)
{
	/* Identity transform, +Z light, red material/ambient and the reference
	 * 1024x768 viewport. Mesa replaces these per draw. Use the actual loader
	 * and its reference CALLs, then verify ALL uploaded words, not just END.
	 */
	static const u32 constants[28] = {
		0x3f800000, 0, 0, 0,
		0, 0x3f800000, 0, 0,
		0, 0, 0x3f800000, 0,
		0, 0, 0, 0x3f800000,
		0, 0, 0x3f800000, 0,
		0x3f800000, 0, 0, 0x3f800000,
		0x3e4ccccd, 0, 0, 0x3f800000,
	};
	static const u32 viewport[8] = {
		0x44000000, 0xc3c00000, 0x3f000000, 0x3f800000,
		0x44000000, 0x43c00000, 0x3f000000, 0x44400000,
	};
	u32 commands[81], result[4], group, vector;
	int ret;

	parhelia_loader_commands(commands, parhelia_warp_entry(pdev, PARHELIA_WARP_CONSTANT_LOADER_ENTRY),
				constants, viewport);
	ret = warp_run_commands(pdev, commands, parhelia_loader_offsets, 5);
	if (ret)
		return ret;
	for (group = 0; group < 3; group++) {
		u32 first = group == 0 ? 0 : group == 1 ? 8 : 0x160;
		u32 count = group == 0 ? 7 : 2;
		const u32 *expected = group == 0 ? constants : viewport;

		for (vector = 0; vector < count; vector++) {
			warp_read_constant(pdev, first + vector, result);
			if (memcmp(result, expected + vector * 4, sizeof(result))) {
				dev_err(&pdev->pdev->dev,
					"WARP loader c%03x mismatch: %08x %08x %08x %08x\n",
					first + vector, result[0], result[1], result[2], result[3]);
				return -EIO;
			}
		}
	}
	dev_info(&pdev->pdev->dev, "WARP loader initialized and verified 11 constant vectors\n");
	return 0;
}

static int warp_initialize_setup_constants(struct parhelia_device *pdev)
{
	/* Constructor defaults from parhelia-block-state-map.md section1.c.
	 * The draw validator's size range replaces the constructor's1..1 range.
	 */
	static const u32 setup[52] = {
		0x3f800000, 0, 0, 0x3f800000,
		0, 0x45000000, 0x3f000000, 0xbf000000,
		0x45ff0000, 0x45ff0000, 0xc5ff0000, 0xc5ff0000,
		0x43000000, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0x3f000000, 0x49400000,
		0xbf800000, 0x3e75fdf0, 0xbd635847, 0x3c1d955b,
		0x3f800000, 0xbaaec3ff, 0x39218489, 0xb77fe5fe,
		0x3fb8aa3b, 0xbf38aa3b, 0x3ef6384f, 0xbeb8aa3b,
		0x3f800000, 0x3e93bb63, 0xbe76384f, 0x3e530bb1,
		0x4b000000, 0x3d4ccccd, 0x41f80000, 0x3d042108,
		0, 0x3d800000, 0, 0,
	};
	u32 commands[88] = {
		3, 0x212c1000, 0, 4, 0x0002305c,
		3, 0x212e2e2d, 0, 0x83c00000, 0,
		4, 0x403c3cdd, 0, 0x20000028, 0x20000028,
		parhelia_warp_entry(pdev, PARHELIA_WARP_CONSTANT_LOADER_ENTRY),
		0x411a0037, 13, 0x172, 1, 0,
	};
	static const u32 offsets[] = { 0, 5, 10, 16, 82, 88 };
	static const u32 tail[] = {
		/* Explicit zero snap constant, not stale context. */
		0x41020007, 1, 0x168, 1, 0, 0, 0, 0, 0,
		4, 0x4c3c3cdd, 0, 0x20000028, 0x20000028, 0x0200300c,
	};
	u32 i, result[4];
	int ret;

	memcpy(commands + 21, setup, sizeof(setup));
	memcpy(commands + 73, tail, sizeof(tail));
	ret = warp_run_commands(pdev, commands, offsets, 5);
	if (ret)
		return ret;
	for (i = 0; i < 13; i++) {
		warp_read_constant(pdev, 0x172 + i, result);
		if (memcmp(result, setup + i * 4, sizeof(result))) {
			dev_err(&pdev->pdev->dev, "WARP setup constant c%x mismatch\n", 0x172 + i);
			return -EIO;
		}
	}
	warp_read_constant(pdev, 0x168, result);
	if (result[0] || result[1] || result[2] || result[3])
		return -EIO;
	dev_info(&pdev->pdev->dev, "WARP setup constants verified: 13 vectors and zero snap\n");
	return 0;
}

static int warp_initialize_context(struct parhelia_device *pdev)
{
	u32 commands[73];
	int ret;

#define ENTRY(which) parhelia_warp_entry(pdev, PARHELIA_WARP_##which##_ENTRY)
	parhelia_publish_commands(commands, ENTRY(PUBLISH), ENTRY(KIND12),
				  ENTRY(KIND6), ENTRY(STAGE_C));
	ret = warp_run_commands(pdev, commands, parhelia_publish_offsets, 5);
	if (ret)
		return ret;
	dev_info(&pdev->pdev->dev, "WARP resident-entry publication completed\n");
	ret = warp_initialize_constants(pdev);
	if (ret)
		return ret;
	ret = warp_initialize_setup_constants(pdev);
	if (ret)
		return ret;
	parhelia_setup_commands(commands, ENTRY(BASE), ENTRY(KIND6), ENTRY(PASSTHROUGH),
			       ENTRY(STAGE_A), ENTRY(STAGE_B), ENTRY(STAGE_C));
	ret = warp_run_commands(pdev, commands, parhelia_setup_offsets, 8);
	if (!ret)
		dev_info(&pdev->pdev->dev, "WARP base setup and A/B/A/B/C stage binding completed\n");
#undef ENTRY
	return ret;
}

int parhelia_warp_init(struct parhelia_device *pdev)
{
	dma_addr_t allocation_bus;
	void *allocation;
	u32 pad;
	int ret = warp_gate_ready(pdev);

	/* Keep both ordinary and diagnostic programs out of the resident area
	 * and each other's allocations. Entries are instruction indices * 4.
	 */
#define WARP_FITS(entry, code, limit) \
	static_assert((entry) / 4 + ARRAY_SIZE(code) / 3 <= (limit) / 4)
	static_assert(PARHELIA_WARP_PASSTHROUGH_ENTRY / 4 >= PARHELIA_WARP_RESIDENT_END);
	WARP_FITS(PARHELIA_WARP_PASSTHROUGH_ENTRY, warp_vertex_probe, PARHELIA_WARP_TEXTURED_ENTRY);
	WARP_FITS(PARHELIA_WARP_TEXTURED_ENTRY, warp_textured,
		  PARHELIA_WARP_TEXTURED_MULTITEX_ENTRY);
	WARP_FITS(PARHELIA_WARP_TEXTURED_MULTITEX_ENTRY, warp_textured_multitex,
		  PARHELIA_WARP_TRANSFORM_ENTRY);
	WARP_FITS(PARHELIA_WARP_TRANSFORM_ENTRY, warp_transform, PARHELIA_WARP_LIGHTING_ENTRY);
	WARP_FITS(PARHELIA_WARP_LIGHTING_ENTRY, warp_lighting, PARHELIA_WARP_REPLACE_ENTRY);
	WARP_FITS(PARHELIA_WARP_REPLACE_ENTRY, warp_replace, PARHELIA_WARP_CONSTANT_LOADER_ENTRY);
	WARP_FITS(PARHELIA_WARP_CONSTANT_LOADER_ENTRY, warp_constant_loader, PARHELIA_WARP_PROBE_ENTRY);
	WARP_FITS(PARHELIA_WARP_PROBE_ENTRY, warp_constant_probe, PARHELIA_WARP_STORE_INSTRUCTIONS * 4);
#undef WARP_FITS

	if (ret)
		return ret;
	if (vertex_probe && !warp_probe)
		return -EINVAL;
	allocation = dmam_alloc_coherent(&pdev->pdev->dev,
		PARHELIA_WARP_STORE_INSTRUCTIONS * 12 + 383,
		&allocation_bus, GFP_KERNEL);
	if (!allocation)
		return -ENOMEM;
	/* The vendor allocator uses division, NOT a power-of-two ALIGN mask. */
	pad = (384 - allocation_bus % 384) % 384;
	pdev->warp_cpu = allocation + pad;
	pdev->warp_bus = allocation_bus + pad;
	ret = warp_load_residents(pdev);
	if (ret)
		return ret;
	if (vertex_probe)
		warp_upload(pdev, PARHELIA_WARP_PASSTHROUGH_ENTRY / 4,
			    warp_vertex_probe, ARRAY_SIZE(warp_vertex_probe));
	else
		warp_upload(pdev, PARHELIA_WARP_PASSTHROUGH_ENTRY / 4,
			    warp_passthrough, ARRAY_SIZE(warp_passthrough));
	warp_upload(pdev, PARHELIA_WARP_TEXTURED_ENTRY / 4, warp_textured, ARRAY_SIZE(warp_textured));
	warp_upload(pdev, PARHELIA_WARP_TEXTURED_MULTITEX_ENTRY / 4,
		    warp_textured_multitex, ARRAY_SIZE(warp_textured_multitex));
	warp_upload(pdev, PARHELIA_WARP_TRANSFORM_ENTRY / 4, warp_transform, ARRAY_SIZE(warp_transform));
	warp_upload(pdev, PARHELIA_WARP_LIGHTING_ENTRY / 4, warp_lighting, ARRAY_SIZE(warp_lighting));
	warp_upload(pdev, PARHELIA_WARP_REPLACE_ENTRY / 4, warp_replace, ARRAY_SIZE(warp_replace));
	warp_upload(pdev, PARHELIA_WARP_CONSTANT_LOADER_ENTRY / 4,
		    warp_constant_loader, ARRAY_SIZE(warp_constant_loader));
	if (warp_probe)
		warp_upload(pdev, PARHELIA_WARP_PROBE_ENTRY / 4,
			    warp_constant_probe, ARRAY_SIZE(warp_constant_probe));
	dma_wmb();
	/* All instruction fetches now use the owned class3 DMA heap. */
	parhelia_write(pdev, 0x0920, parhelia_read(pdev, 0x0920) | 1u);
	if (warp_probe) {
		static const u32 inputs[2][4] = {
			{ 0x3e000000, 0x3e800000, 0x3f000000, 0x3f800000 },
			{ 0x3f800000, 0x3f000000, 0x3e800000, 0x3e000000 },
		};
		u32 i;

		for (i = 0; i < ARRAY_SIZE(inputs); i++) {
			ret = warp_probe_once(pdev, inputs[i]);
			if (ret)
				return ret;
		}
		dev_info(&pdev->pdev->dev, "WARP constant-store probe passed both patterns\n");
	}
	return warp_initialize_context(pdev);
}
