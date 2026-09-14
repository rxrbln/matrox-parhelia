/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Userspace ABI for the Matrox Parhelia render node.
 *
 * The primary DMA ring is deliberately absent from this ABI.  It must be
 * DMA32-coherent memory owned by the kernel; exposing it would let one client
 * point the bus master at another client's memory.  Userspace supplies only
 * header-ful secondary packets and their packet boundaries.  The kernel
 * checks framing and relocates them into its primary ring. Submission is
 * is valid on a DRM render node. Register packets are framed and checked,
 * address-bearing operands are GEM relocations, and the primary ring and
 * DMA writeback memory always remain kernel-owned.
 */
#ifndef __PARHELIA_DRM_H__
#define __PARHELIA_DRM_H__

#include <uapi/drm/drm.h>

#define DRM_PARHELIA_GET_PARAM	0x00
#define DRM_PARHELIA_SUBMIT		0x01
#define DRM_PARHELIA_CREATE_RENDER	0x02
#define DRM_PARHELIA_CREATE_VERTEX	0x03
#define DRM_PARHELIA_WAIT_FENCE		0x04
#define DRM_PARHELIA_GET_BO_INFO	0x05
#define DRM_PARHELIA_UPLOAD_RENDER	0x06
#define DRM_PARHELIA_DOWNLOAD_RENDER	0x07
#define DRM_PARHELIA_WARP_PROGRAM	0x08
#define DRM_PARHELIA_M9_CREATE_BO	0x09
#define DRM_PARHELIA_M9_SUBMIT		0x0a

#define DRM_IOCTL_PARHELIA_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_GET_PARAM, \
		 struct drm_parhelia_get_param)
#define DRM_IOCTL_PARHELIA_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_SUBMIT, \
		 struct drm_parhelia_submit)
#define DRM_IOCTL_PARHELIA_CREATE_RENDER \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_CREATE_RENDER, \
		 struct drm_parhelia_create_render)
#define DRM_IOCTL_PARHELIA_CREATE_VERTEX \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_CREATE_VERTEX, \
		 struct drm_parhelia_create_vertex)
#define DRM_IOCTL_PARHELIA_WAIT_FENCE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_WAIT_FENCE, \
		 struct drm_parhelia_wait_fence)
#define DRM_IOCTL_PARHELIA_GET_BO_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_GET_BO_INFO, \
		 struct drm_parhelia_get_bo_info)
#define DRM_IOCTL_PARHELIA_UPLOAD_RENDER \
	DRM_IOW(DRM_COMMAND_BASE + DRM_PARHELIA_UPLOAD_RENDER, \
		struct drm_parhelia_upload_render)
#define DRM_IOCTL_PARHELIA_DOWNLOAD_RENDER \
	DRM_IOW(DRM_COMMAND_BASE + DRM_PARHELIA_DOWNLOAD_RENDER, \
		struct drm_parhelia_upload_render)
#define DRM_IOCTL_PARHELIA_WARP_PROGRAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_WARP_PROGRAM, \
		 struct drm_parhelia_warp_program)
#define DRM_IOCTL_PARHELIA_M9_CREATE_BO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_CREATE_BO, \
		 struct drm_parhelia_m9_create_bo)
#define DRM_IOCTL_PARHELIA_M9_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_SUBMIT, \
		 struct drm_parhelia_m9_submit)

/* Linear storage does not use the tiled render-target address decoder and
 * therefore needs only the native surface alignment, not a 32 MiB SUN90
 * bank placement. */
#define PARHELIA_CREATE_RENDER_LINEAR	(1u << 0)
/* A layout-2 sampler image is addressed through the texture unit rather than
 * the SUN90 render-target path.  It needs tiled storage, but not the 32 MiB
 * render-target bank separation. */
#define PARHELIA_CREATE_RENDER_TEXTURE	(1u << 1)
/* P690 native render-target classes.  The recovered vendor allocator keeps
 * colour and depth in separate 32 MiB policy bands while packing each class
 * at the hardware's 8 KiB surface alignment. */
#define PARHELIA_CREATE_RENDER_NATIVE_COLOR	(1u << 3)
#define PARHELIA_CREATE_RENDER_NATIVE_DEPTH	(1u << 4)
/* Native colour layout for a front-rendered display buffer, placed in the
 * third 32 MiB band (bank B) so display refresh does not compete with the
 * colour and texture traffic of bank A. */
#define PARHELIA_CREATE_RENDER_NATIVE_SCANOUT	(1u << 5)

/* Bring-up tiled render allocation, deliberately separate from linear dumb
 * scanout. Both 32-bit colour and S8Z24 use this storage. The kernel reserves
 * a conservative non-aliasing extent and 8KiB placement alignment; @size is
 * that reserved extent, not width*height*4. No GPU addresses are exported.
 * Currently supported logical extent: at most 2048x2048.
 * Mapping requires the kernel-owned raw host-aperture baseline.
 */
struct drm_parhelia_create_render {
	__u32 width;
	__u32 height;
	__u32 flags;
	__u32 handle; /* out */
	__u32 pitch; /* out: bytes, tile width aligned */
	/* Bytes per pixel: 0 and 4 are the 32-bit formats, 2 selects R5G6B5
	 * colour or D16 depth.  A 4096-byte tile holds 128x16 pixels at two
	 * bytes instead of 64x16, so the pitch aligns to 128 pixels. */
	__u32 cpp;
	__u64 size;  /* out: allocation bytes */
	__u64 mmap_offset; /* out: DRM file mmap offset, NOT a GPU address */
};

/* A linear, DMA32 vertex allocation.  This is deliberately distinct from a
 * render target and is never mapped through BAR0.  The driver owns its class-3
 * bus address and accepts it only for bounded draw relocations from the
 * creating render file.  @size is both the requested and returned allocation
 * size; @mmap_offset is a DRM file mapping offset and never a GPU address. */
struct drm_parhelia_create_vertex {
	__u64 size;
	__u32 flags;
	__u32 handle; /* out */
	__u64 mmap_offset; /* out: DRM file mmap offset */
};

#define PARHELIA_BO_LAYOUT_LINEAR		0
#define PARHELIA_BO_LAYOUT_NATIVE_64X32	1
#define PARHELIA_BO_LAYOUT_NATIVE_64X16	2
#define PARHELIA_BO_LAYOUT_TEXTURE		3
#define PARHELIA_BO_INFO_HOST_APERTURE	(1u << 0)

struct drm_parhelia_get_bo_info {
	__u32 handle;
	__u32 flags;
	__u32 layout;
	__u32 width;
	__u32 height;
	__u32 pitch; /* out: native GPU byte pitch, not dumb host pitch */
	__u32 pad;
	__u64 size;
};

/* Copy an already hardware-laid-out byte span between userspace and native
 * storage.  UPLOAD reads from @data; DOWNLOAD writes to @data.  Both ioctls
 * use this structure and require four-byte-aligned bounds.
 * The kernel creates a short-lived type-0 host-aperture view for the copy;
 * userspace never receives a raw BAR mapping of local memory. */
struct drm_parhelia_upload_render {
	__u64 data;
	__u64 offset;
	__u64 size;
	__u32 handle;
	__u32 flags;
	__u64 pad;
};

/* Native sharing is intentionally same-device: FLINK/OPEN or a self-imported
 * PRIME fd preserves object identity for GET_BO_INFO.  The physical layout is
 * not DRM_FORMAT_MOD_LINEAR and is not portable to another DRM device.
 */

#define PARHELIA_PARAM_CHIP_ID		0
#define PARHELIA_PARAM_VRAM_SIZE	1
#define PARHELIA_PARAM_MAX_PACKETS	2
#define PARHELIA_PARAM_MAX_COMMAND_DWORDS	3
#define PARHELIA_PARAM_WARP_PASSTHROUGH	4
#define PARHELIA_PARAM_WARP_TEXTURED	5
#define PARHELIA_PARAM_WARP_REPLACE	6
#define PARHELIA_PARAM_WARP_TRANSFORM	7
#define PARHELIA_PARAM_WARP_LIGHTING	8
#define PARHELIA_PARAM_WARP_CONSTANT_LOADER	9
#define PARHELIA_PARAM_TILE_LAYOUT	10
#define PARHELIA_PARAM_LAST_CLOCK_KHZ	11
#define PARHELIA_PARAM_TEXTURE_UNITS	12
#define PARHELIA_PARAM_TEXTURE_PIPE_MODE	13
/* WARP pass-through for two texture-coordinate sets: o3 <- v7, o4 <- v8. */
#define PARHELIA_PARAM_WARP_TEXTURED_MULTITEX	14
/* Non-zero only when CREATE_VERTEX's dedicated class-3 DMA buffers are usable.
 * The old name is retained for source and binary ABI compatibility. */
#define PARHELIA_PARAM_VRAM_VERTEX	15
#define PARHELIA_PARAM_VERTEX_BUFFER	PARHELIA_PARAM_VRAM_VERTEX
/* P690-only guarded mode-7 index/stream relocation contract. */
#define PARHELIA_PARAM_NATIVE_INDEXED	16
/* Non-zero only when the fenced asynchronous producer queue is available. */
#define PARHELIA_PARAM_ASYNC_SUBMIT	17
/* P690-only guarded sequential bound-stream range diagnostic. */
#define PARHELIA_PARAM_BOUND_RANGE_TEST	18
/* Production one-triangle-per-packet mode-3 over CREATE_VERTEX storage. */
#define PARHELIA_PARAM_BOUND_RANGE	19
/* Kernel can insert owned EOB/DWGSYNC brackets at batch-boundary relocs. */
#define PARHELIA_PARAM_STATEFUL_SUBMIT	20
#define PARHELIA_PARAM_NATIVE_SHARE	21
/* Guarded opaque type-0 host-aperture texture upload probe. */
#define PARHELIA_PARAM_TEXTURE_UPLOAD	22
/* Number of requests the kernel can retain in its ordered async queue. */
#define PARHELIA_PARAM_ASYNC_SLOTS	23
/* Retired unsafe generic packing capability; kernels report zero. */
#define PARHELIA_PARAM_PACK_ADJACENT	24
/* Boundary-aware adjacent-packet packing and CALL_BOUNDARY relocations. */
#define PARHELIA_PARAM_PACK_ADJACENT_BOUNDARIES	25
/* P690 exposes separate packed native colour/depth allocation classes. */
#define PARHELIA_PARAM_NATIVE_RT_CLASSES	27
/* Safe bounded UPLOAD_RENDER and DOWNLOAD_RENDER through a kernel aperture. */
#define PARHELIA_PARAM_RENDER_TRANSFER	28
/* Hardware transform-and-lighting: warp_transform with the o3 (and o4)
 * texture-coordinate pass-through, so a transformed draw can be textured. */
#define PARHELIA_PARAM_WARP_TRANSFORM_TEXTURED	29
#define PARHELIA_PARAM_WARP_TRANSFORM_MULTITEX	30
/* Instructions one WARP_PROGRAM slot holds, zero when the ioctl is absent. */
#define PARHELIA_PARAM_WARP_PROGRAM_INSTRUCTIONS	31
/* WARP program slots this file may hold at once. */
#define PARHELIA_PARAM_WARP_PROGRAM_SLOTS	32

#define PARHELIA_TEXTURE_PIPES_PAIRED	0
#define PARHELIA_TEXTURE_PIPES_SINGLE	1

/* Physical 32-bpp render-target layout selected by the kernel from the PCI
 * silicon id.  These are layout ABI values, not memory-controller guesses. */
#define PARHELIA_TILE_64X32_8K		0
#define PARHELIA_TILE_64X16_4K		1

struct drm_parhelia_get_param {
	__u64 value;
	__u32 param;
	__u32 pad;
};

/* One secondary CALL span in @commands, in dwords. Usually one packet;
 * consecutive constant-file 0x41 packets may share a span, preserving the
 * reference constant group's single CALL. Other packet kinds stay separate.
 */
struct drm_parhelia_packet {
	__u32 offset;
	__u32 length;
};

/*
 * A relocation names an address-bearing command operand.  Raw device and bus
 * addresses are deliberately not part of the ABI: the kernel translates a
 * GEM handle to a pinned VRAM offset, or a range in the submit's DMA32 vertex
 * upload.  @packet is an index into drm_parhelia_submit.packets.
 *
 * For PARHELIA_RELOC_VRAM, @dword is the packet-relative operand to replace,
 * @handle identifies the GEM VRAM object and @delta is a byte offset in it.
 * For PARHELIA_RELOC_DRAW, @packet must identify a 0x97 packet; @dword and
 * @delta are respectively the byte offset and byte length in @vertices.
 *
 * PARHELIA_RELOC_DRAW_VRAM uses the same range fields, but names a dedicated
 * class-3 DMA allocation returned by CREATE_VERTEX.  The old suffix is kept
 * for binary ABI compatibility.  The kernel validates allocation ownership
 * and the complete byte range; the bus address never crosses the UAPI.
 *
 * PARHELIA_RELOC_STREAM_VRAM is reserved for a mode-7 direct-fetch stream.
 * @packet identifies the opcode-0 register-list packet, @dword is the
 * packet-relative stream-base data operand, @handle is a creator-owned
 * CREATE_VERTEX allocation, and @delta is its four-byte-aligned byte offset.
 * PARHELIA_RELOC_INDEX_VRAM identifies a three-dword mode-7 packet; @dword
 * is the index-array byte offset within @handle and @delta is its nonzero
 * four-byte-aligned byte length.  The kernel must validate the complete
 * stream/descriptor/floor/limit contract and every u32 ordinal before
 * accepting either relocation.  These types intentionally carry no raw
 * device address.
 *
 * PARHELIA_RELOC_STREAM_DMA and PARHELIA_RELOC_INDEX_DMA are reserved for
 * the isolated P690 mode-7 transport test. They select byte slices of this
 * submit's copied @vertices blob; @handle must be zero. The kernel owns the
 * resulting coherent DMA32 allocation and supplies its class-3 addresses
 * only after it has validated the direct-fetch stream and every ordinal.
 * PARHELIA_RELOC_RANGE_STREAM_DMA is narrower still: it binds a fixed
 * six-vertex, five- or six-dword stream used by the isolated mode-3 screen
 * test.  It is never advertised to normal rendering clients.
 * PARHELIA_RELOC_RANGE_STREAM_VRAM binds the production mode-3 stream to a
 * creator-owned CREATE_VERTEX allocation.  @delta is its byte offset.  Each
 * 0x8f packet remains one triangle so its packet boundary preserves depth
 * interlock.
 *
 * PARHELIA_RELOC_BATCH_BOUNDARY carries no address.  @packet names the first
 * packet of a later logical state batch concatenated into this submit and all
 * other fields must be zero.  The kernel replaces the marker with its owned
 * EOB/DWGSYNC drain followed by the next opening bracket.  This preserves the
 * vendor's per-state-validate ordering while allowing one ioctl and one final
 * primary producer update to carry two different consecutive states.
 *
 * PARHELIA_RELOC_CALL_BOUNDARY also carries no address.  @packet names the
 * first packet of a later logical state section; all other fields must be
 * zero.  Unlike BATCH_BOUNDARY, it emits no EOB, DWGSYNC or opening bracket.
 * It only prevents boundary-aware adjacent-packet packing from placing
 * packets on opposite sides in the same secondary CALL.
 */
struct drm_parhelia_reloc {
	__u32 packet;
	__u32 dword;
	__u32 handle;
	__u32 delta;
	__u32 type;
	__u32 pad[3];
};

#define PARHELIA_RELOC_VRAM	0u
#define PARHELIA_RELOC_DRAW	1u
#define PARHELIA_RELOC_DRAW_VRAM	2u
#define PARHELIA_RELOC_STREAM_VRAM	3u
#define PARHELIA_RELOC_INDEX_VRAM	4u
#define PARHELIA_RELOC_STREAM_DMA	5u
#define PARHELIA_RELOC_INDEX_DMA	6u
#define PARHELIA_RELOC_RANGE_STREAM_DMA	7u
#define PARHELIA_RELOC_RANGE_STREAM_VRAM	8u
#define PARHELIA_RELOC_BATCH_BOUNDARY	9u
#define PARHELIA_RELOC_CALL_BOUNDARY	10u

/*
 * Submit a sequence of header-ful secondary packets.
 *
 * @commands is copied into a kernel-owned DMA32-coherent buffer.  Every
 * span is independently framed by a kernel-generated primary CALL, so the
 * engine never consumes unvalidated primary-ring words from userspace.
 *
 * The submit completes synchronously.  This is intentional for the initial
 * render ABI: it permits the small per-device DMA command and vertex upload
 * buffers to be reused without exposing an unfenced address to the engine.
 * The kernel owns EOB and fence generation; clients must not write index
 * 0xaf (0x12bc), DMA writeback addresses, or nested secondary CALLs.
 */
struct drm_parhelia_submit {
	__u64 commands;
	__u64 packets;
	__u64 relocs;
	__u64 vertices;
	__u32 command_dwords;
	__u32 packet_count;
	__u32 reloc_count;
	__u32 vertex_dwords;
	__u32 flags;
	__u32 fence;		/* out: monotonically increasing fence tag */
};

#define PARHELIA_SUBMIT_FENCE	(1u << 0)
/* Return after the producer doorbell. PARHELIA_SUBMIT_FENCE is mandatory. */
#define PARHELIA_SUBMIT_ASYNC	(1u << 1)
/* CALL_BOUNDARY markers completely describe this submit's logical sections. */
#define PARHELIA_SUBMIT_CALL_BOUNDARIES	0x4u

/* Wait for a terminal submit fence. timeout_ns is relative: zero polls and
 * -1 requests an interruptible unbounded wait. completed always reports the
 * current masked DWGSYNC value, including on a timeout. */
/*
 * Upload one caller-owned WARP vertex program and bind it to a kernel-owned
 * instruction-store slot.
 *
 * @code points at @count 96-bit bundles, three little-endian dwords each.
 * @slot is zero on the first call and is then filled in with the slot the
 * kernel allocated for this file; passing it back re-uploads the same slot.
 * @entry receives the entry token to write to WARP_ENTRY; it is only valid
 * for submits from this file and stops being accepted when the slot is
 * released.  PARHELIA_WARP_PROGRAM_FREE releases @slot and uploads nothing.
 *
 * The kernel validates every bundle.  A program runs on the vertex engine
 * with access to the constant file, which is also the engine's block-state
 * file, so the accepted subset is deliberately narrow: arithmetic opcodes
 * only, no control class and therefore no loops, no relative addressing, no
 * constant-file destination, and every register index bounded.  Rejected
 * programs leave the slot's previous contents untouched.
 */
struct drm_parhelia_warp_program {
	__u64 code;
	__u32 count;		/* instructions, not dwords */
	__u32 flags;
	__u32 slot;		/* in/out: 0 allocates */
	__u32 pad;
	__u64 entry;		/* out: WARP_ENTRY token */
};

#define PARHELIA_WARP_PROGRAM_FREE	(1u << 0)

struct drm_parhelia_wait_fence {
	__u32 fence;
	__u32 flags;
	__s64 timeout_ns;
	__u32 completed;
	__u32 pad;
};

/* M9 command and staging objects are immutable DMA32 allocations. The mmap
 * offset is a DRM cookie and no engine address is exposed. */
struct drm_parhelia_m9_create_bo {
	__u64 size;
	__u64 mmap_offset; /* out */
	__u32 flags;
	__u32 handle; /* out */
};

#define PARHELIA_M9_BO_COMMAND	(1u << 0)
#define PARHELIA_M9_BO_READ	(1u << 1)
#define PARHELIA_M9_BO_WRITE	(1u << 2)

struct drm_parhelia_m9_bo_ref {
	__u32 handle;
	__u32 flags;
};

enum drm_parhelia_m9_reloc_kind {
	PARHELIA_M9_RELOC_REQUESTER,
	PARHELIA_M9_RELOC_COLOR,
	PARHELIA_M9_RELOC_DEPTH,
	PARHELIA_M9_RELOC_TEXTURE,
	PARHELIA_M9_RELOC_PROGRAM,
	PARHELIA_M9_RELOC_DRAW32_START,
	PARHELIA_M9_RELOC_DRAW32_END,
	PARHELIA_M9_RELOC_DRAW16_START,
	PARHELIA_M9_RELOC_DRAW16_END,
	PARHELIA_M9_RELOC_FENCE,
	PARHELIA_M9_RELOC_QUERY,
	PARHELIA_M9_RELOC_SPECIAL,
	/* Index 0x6c: 0x80-aligned linked VS+FS record; kernel adds tag bit 0. */
	PARHELIA_M9_RELOC_PROGRAM_LINK,
};

struct drm_parhelia_m9_reloc {
	__u32 command_dword;
	__u32 handle;
	__u64 target_offset;
	__u32 kind;
	__u32 flags;
	__u32 pad[2];
};

#define PARHELIA_M9_SUBMIT_VALIDATE_ONLY	(1u << 0)
#define PARHELIA_M9_SUBMIT_NOOP_FENCE		(1u << 1)

/* The kernel copies and relocates the command slice into its own DMA32
 * allocation. NOOP_FENCE ignores command fields and submits only the
 * kernel-owned completion record. */
struct drm_parhelia_m9_submit {
	__u32 context_id;
	__u32 command_handle;
	__u64 command_offset;
	__u32 command_length;
	__u32 relocation_count;
	__u64 relocations_ptr;
	__u32 bo_count;
	__u32 flags;
	__u64 bos_ptr;
	__u32 out_syncobj;
	__u32 pad;
	__u64 out_point;
};

#endif
