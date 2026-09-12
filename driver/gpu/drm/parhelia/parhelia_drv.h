/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __PARHELIA_DRV_H__
#define __PARHELIA_DRV_H__

#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/dma-fence.h>
#include <linux/bitmap.h>
#include <linux/i2c-algo-bit.h>
#include <linux/i2c.h>

#include <drm/drm_device.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_mm.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <linux/iosys-map.h>

#include "parhelia_drm.h"
#include "parhelia_packets.h"

struct drm_file;
struct drm_gem_vram_object;

#define PARHELIA_MAX_OUTPUTS	3

#define PARHELIA_VENDOR		0x102b
#define PARHELIA_512		0x0527
#define PARHELIA_8X		0x0528
#define PARHELIA_LX		0x2537
#define PARHELIA_LX2		0x2538
#define PARHELIA_P690		0x2539

enum parhelia_mc_family {
	PARHELIA_MC_HSLPAR,
	PARHELIA_MC_SUN90,
};

enum parhelia_clock_family {
	PARHELIA_CLOCK_HSLPAR,
	PARHELIA_CLOCK_SUN90,
};

enum parhelia_output_family {
	PARHELIA_OUTPUT_ORIGINAL,
	PARHELIA_OUTPUT_LX,
	PARHELIA_OUTPUT_P690,
};

enum parhelia_reset_family {
	PARHELIA_RESET_HSLPAR,
	PARHELIA_RESET_SUNEX,
};

struct parhelia_chip_info {
	u16 device;
	const char *name;
	u8 tile_layout;
	u8 texture_units;
	u8 texture_pipe_mode;
	enum parhelia_mc_family mc_family;
	enum parhelia_clock_family clock_family;
	enum parhelia_output_family output_family;
	enum parhelia_reset_family reset_family;
	u32 max_pixel_clock_khz;
};

enum parhelia_output_route {
	PARHELIA_ROUTE_DVO1,
	PARHELIA_ROUTE_DVO2,
	PARHELIA_ROUTE_DAC1,
	PARHELIA_ROUTE_DAC2,
};

/* Physical connector wiring.  All native DDC channels use 0x1c48; each
 * descriptor selects its own GPIO pin pair.  ready is deliberately separate
 * from the wiring data so unverified connectors can be described without
 * advertising a connector whose output path KMS cannot drive yet. */
struct parhelia_output_desc {
	u8 connector_type;
	u8 ddc_line;
	u8 ddc_scl;
	u8 ddc_sda;
	u8 preferred_head;
	u8 digital_route;
	u8 analog_route;
	/* expose registers DDC/detect/EDID; ready also attaches the connector to
	 * the currently implemented display pipe. */
	bool expose;
	bool ready;
};

struct parhelia_output {
	struct drm_connector connector;
	struct i2c_adapter ddc;
	struct i2c_algo_bit_data ddc_bit;
	struct parhelia_device *pdev;
	const struct parhelia_output_desc *desc;
	u8 ddc_scl;
	u8 ddc_sda;
	bool edid_valid;
	bool edid_digital;
	bool analog_sensed;
};

/* PInS fields retained after the PCI option ROM is unmapped. */
struct parhelia_bios {
	u32 ddc_pin_ids;		/* byte index is the 0x1c48 GPIO bit */
	u16 pins_version;
	u16 pins_offset;
	bool pins_valid;
	bool ddc_pin_ids_from_rom;
};

#define PARHELIA_MMIO_RING_START	0x0404
#define PARHELIA_MMIO_RING_END		0x0408
#define PARHELIA_MMIO_FENCE_DONE	0x041c
#define PARHELIA_MMIO_WRITEBACK		0x0400
#define PARHELIA_MMIO_IRQ_STATUS	0x08f4
#define PARHELIA_MMIO_IRQ_ACK		0x08f8
#define PARHELIA_MMIO_IRQ_MASK		0x08fc
/* Engine source enabled by the vendor Linux driver after the ring goes live
 * (the soft-trap/DWGSYNC completion). */
#define PARHELIA_IRQ_ENGINE		BIT(16)
/* The vendor's OsSetVSyncInterruptState accepts 0x2 for head 0. */
#define PARHELIA_IRQ_VBLANK_HEAD0	BIT(1)

#define PARHELIA_DC_BASE		0x1800
#define PARHELIA_DC_STRIDE		0x200
#define PARHELIA_DC_HEAD0		0
#define PARHELIA_DC_HEAD1		1
#define PARHELIA_DC(head)		(PARHELIA_DC_BASE + (head) * PARHELIA_DC_STRIDE)
#define PARHELIA_DCT(head)		(0x1900 + (head) * PARHELIA_DC_STRIDE)
#define PARHELIA_DC_CTRL		0x000
#define PARHELIA_DC_XPAN		0x004
#define PARHELIA_DC_YPAN		0x048
#define PARHELIA_DC_START		0x024
#define PARHELIA_DC_FORMAT		0x034

/* Host-aperture surface descriptors.  BAR0 accesses falling inside a valid
 * descriptor are translated between a CPU-linear view and the surface layout
 * named by CTRL.  Slot 0 remains the engine bootstrap; dumb BOs own slots
 * 1..19 for their whole mapped/pinned lifetime.
 */
#define PARHELIA_HOST_APERTURE_SLOT(n)	((n) * 0x10)
#define PARHELIA_LOCAL_BANK_B_BASE	0x08000000u
#define PARHELIA_HOST_APERTURE_BASE	0x10000000u
#define PARHELIA_HOST_APERTURE_VALID	BIT(28)
#define PARHELIA_HOST_APERTURE_SLOTS	20
#define PARHELIA_HOST_APERTURE_ALIGN	SZ_256K
#define PARHELIA_HOST_APERTURE_LIMIT	SZ_16M
#define PARHELIA_HOST_APERTURE_FIRST	(2 * PARHELIA_HOST_APERTURE_ALIGN)
#define PARHELIA_DC_LUT_INDEX		0x038
#define PARHELIA_DC_LUT_R		0x03c
#define PARHELIA_DC_LUT_G		0x040
#define PARHELIA_DC_LUT_B		0x044
#define PARHELIA_DC_HDISP		0x008
#define PARHELIA_DC_VDISP		0x04c
#define PARHELIA_DC_PARAMS		0x088
#define PARHELIA_DC_LUT_GATE		0x1aa
#define PARHELIA_DC_LUT_LATCH		BIT(31)
#define PARHELIA_DC_LUT_COMPONENT_MASK	0x0000ffc0
#define PARHELIA_DC_CTRL_ON		0x00040201
#define PARHELIA_DC_VIDEO_EN		0x0030
#define PARHELIA_DC_DIM_EN		0x0082
#define PARHELIA_DCT_MISC		0x000
#define PARHELIA_DCT_HTIMING		0x008
#define PARHELIA_DCT_HTIMING2		0x010
#define PARHELIA_DCT_HSYNC		0x014
#define PARHELIA_DCT_VTOTAL		0x018
#define PARHELIA_DCT_VTIMING		0x020
#define PARHELIA_DCT_VBLANK2		0x028
#define PARHELIA_DCT_VTIMING2		0x02c
#define PARHELIA_DCT_VSYNC		0x030
#define PARHELIA_DCT_FIELD		0x034
#define PARHELIA_DCT_RESET		0x00030000
#define PARHELIA_DCM_CLKCTL_HEAD	0x1c08
#define PARHELIA_DCM_CLKCTL_A		0x1c00
#define PARHELIA_DCM_CLKCTL_B		0x1c04
#define PARHELIA_DCM_MODE_HIRES	0x0002
#define PARHELIA_DCM_MODE_VGA		0x0001
#define PARHELIA_DCM_CLKCTL_MUX_MASK	0x0003
#define PARHELIA_DCM_CLKCTL_MUX_PLL	0x0002
#define PARHELIA_DCM_CLKCTL_HOLD	0x0020
#define PARHELIA_DCM_CLKCTL_RSTROUTE	0x0010
#define PARHELIA_DCM_CLKCTL_RSTSTROBE	BIT(24)
#define PARHELIA_DCM_CLKCTL_BIT10	0x0400
#define PARHELIA_DCM_CLKCTL_SEL_MASK	0x03c0
#define PARHELIA_DCM_CLKCTL_SEL_SHIFT	6
#define PARHELIA_DCM_PRIORITY_A		0x1c0c
#define PARHELIA_DCM_PRIORITY_B		0x1c10
#define PARHELIA_DCM_PRIORITY_ON	0x0b0b
#define PARHELIA_VGA_MISC_W		0x1fc2
#define PARHELIA_VGA_MISC_R		0x1fcc
#define PARHELIA_VGA_MISC_CLKSEL_MASK	0x0c
#define PARHELIA_VGA_MISC_CLKSEL_FS	0x08
#define PARHELIA_REG_DVO_XBAR		0x1c20
#define PARHELIA_REG_DVO_PWR		0x1c24
#define PARHELIA_REG_DVO_FORMAT2	0x1c54
#define PARHELIA_REG_DVO_SYNC2		0x1c58
#define PARHELIA_REG_DVO_DATA		0x1c70
#define PARHELIA_REG_DDC		0x1c48
#define PARHELIA_REG_OUTPUT_GATE	0x08f4
#define PARHELIA_OUTPUT_GATE_TMDS	BIT(5)
#define PARHELIA_REG_DAC1		0x1c18
#define PARHELIA_REG_DAC2		0x1c1c
#define PARHELIA_REG_DAC_SENSE		0x1c14
#define PARHELIA_REG_DAC_SENSE_TEST	0x188c
#define PARHELIA_REG_DAC_SENSE_MODE	0x1910
#define PARHELIA_DAC_HSYNC_NEG		BIT(11)
#define PARHELIA_DAC_VSYNC_NEG		BIT(13)
#define PARHELIA_DAC_ROUTE_DCMB		BIT(14)
#define PARHELIA_DAC2_PWR		0x00031400
#define PARHELIA_DAC1_PWR		0x00019400
#define PARHELIA_DAC_SENSE_DISABLE	0x00001400
#define PARHELIA_DAC_SENSE_VALUE	0x16459164
#define PARHELIA_DAC_SENSE_MODE_MASK	0xf000ffff

#define PARHELIA_LEGACY_USER_PACKETS	(PARHELIA_CALL_CAPACITY - 3u)
#define PARHELIA_MAX_USER_PACKETS	PARHELIA_USER_PACKET_CAPACITY
#define PARHELIA_DMA_BLOCK_CAPACITY	PARHELIA_PLAN_CAPACITY
#define PARHELIA_MAX_COMMAND_DWORDS	(64u * 1024u)
/* Five-dword CALL plus optional three-dword diagnostic writeback marker. */
#define PARHELIA_PRIMARY_DWORDS		(PARHELIA_CALL_CAPACITY * 8u + 16u)
/* Queue depth.  ioquake3 at 1280x1024 spent 4.3 ms of a 19.3 ms frame
 * waiting for a slot to retire with eight. */
#define PARHELIA_ASYNC_SLOTS		16u
/* The primary always remains bounded by the hardware CALL capacity, even
 * when one secondary CALL contains several validated packet descriptors.
 */
#define PARHELIA_SLOT_PRIMARY_DWORDS	(PARHELIA_CALL_CAPACITY * 5u + 16u)
#define PARHELIA_ASYNC_PRIMARY_DWORDS	(PARHELIA_ASYNC_SLOTS * PARHELIA_SLOT_PRIMARY_DWORDS)
#define PARHELIA_MAX_RELOCS		(PARHELIA_MAX_USER_PACKETS + 64u)
#define PARHELIA_DMA_BLOCK_DWORDS	4096u
#define PARHELIA_VERTEX_DWORDS		4096u
/* Largest vertex interval proven through one class-3 0x97 fetch.  A submit
 * may contain many independently relocated intervals from the same object. */
#define PARHELIA_MAX_DRAW_DWORDS	4096u
#define PARHELIA_VERTEX_BO_MAX_SIZE	(1u << 20)

/* Heap-relative instruction entries. Runtime GET_PARAM returns the DMA
 * entry, not these offsets; the heap path uses memory class3.
 */
#define PARHELIA_WARP_PASSTHROUGH_ENTRY	(1760u * 4)
#define PARHELIA_WARP_TEXTURED_ENTRY	(1888u * 4)
/* The single-texture program only writes o3.  Keep it available for old
 * userspace and place the two-set variant in the unused part of the same
 * 128-instruction allocation. */
#define PARHELIA_WARP_TEXTURED_MULTITEX_ENTRY	(1896u * 4)
#define PARHELIA_WARP_TRANSFORM_ENTRY	(2016u * 4)
#define PARHELIA_WARP_LIGHTING_ENTRY	(2144u * 4)
#define PARHELIA_WARP_REPLACE_ENTRY	(2272u * 4)
#define PARHELIA_WARP_CONSTANT_LOADER_ENTRY	(2400u * 4)
#define PARHELIA_WARP_PROBE_ENTRY	(2528u * 4)

/* Userspace program heap.  Slot ids are 1-based across the ABI; slot 0 means
 * "not allocated".  One slot holds a whole program including its END and the
 * trailing padding bundle. */
#define PARHELIA_WARP_USER_SLOTS		32u
/* Instruction fetch is host DMA, so the image size is a driver policy choice
 * rather than a hardware limit: 128 bundles per image is what a compiled GLSL
 * vertex shader needs, and the whole heap is 96 KiB of coherent memory. */
#define PARHELIA_WARP_USER_SLOT_INSTRUCTIONS	256u
/* Two program images per slot; an upload never lands on the address the
 * engine most recently fetched from.  See warp_user_entry(). */
#define PARHELIA_WARP_USER_IMAGE_INSTRUCTIONS	128u
#define PARHELIA_WARP_USER_FILE_SLOTS		8u
#define PARHELIA_WARP_USER_INSTRUCTIONS \
	(PARHELIA_WARP_USER_SLOTS * PARHELIA_WARP_USER_SLOT_INSTRUCTIONS)
/* warp_transform with the texture-coordinate pass-through of the textured
 * programs, so a hardware-transformed draw can also be textured. */
#define PARHELIA_WARP_TRANSFORM_TEXTURED_ENTRY	(2656u * 4)
#define PARHELIA_WARP_TRANSFORM_MULTITEX_ENTRY	(2784u * 4)

struct parhelia_dma_block {
	void *cpu;
	dma_addr_t bus;
	/* Async slots size each secondary allocation to its validated packet,
	 * rather than permanently reserving 16 KiB for all 224 possible CALLs. */
	u32 dwords;
};

enum parhelia_slot_state {
	PARHELIA_SLOT_FREE,
	PARHELIA_SLOT_QUEUED,
	/* A hung engine may still consume these allocations.  They are released
	 * only from submit_fini after PCI mastering has been disabled. */
	PARHELIA_SLOT_QUARANTINED,
};

struct parhelia_submit_fence {
	struct dma_fence base;
	spinlock_t lock;
};

struct parhelia_root_probe_trace;

struct parhelia_submit_slot {
	enum parhelia_slot_state state;
	struct drm_file *file;
	u32 fence;
	u32 dma_tag;
	u32 primary_begin;
	u32 primary_end;
	u32 call_count;
	/* Hang budget and submission time; the budget runs from the later of
	 * submission and the last observed fence progress. */
	unsigned long budget;
	unsigned long submitted;
	u64 submit_ns;
	struct parhelia_dma_block *blocks;
	/* A single compact coherent secondary heap backs every packet in one
	 * request. The CPU-only block index may exceed the hardware CALL count
	 * while adjacent packet descriptors are being compacted.
	 */
	void *secondary_cpu;
	dma_addr_t secondary_bus;
	u32 secondary_dwords;
	u32 secondary_put;
	void *vertex_cpu;
	dma_addr_t vertex_bus;
	void *writeback_cpu;
	dma_addr_t writeback_bus;
	struct drm_gem_object **objects;
	struct drm_gem_vram_object **pinned;
	u32 reloc_count;
	struct drm_gem_vram_object *mode7_index_copy;
	struct parhelia_submit_fence *out_fence;
	struct parhelia_root_probe_trace *root_probe;
};

/* CREATE_VERTEX is a dedicated class-3 DMA object.  It must never be backed by
 * the programmable BAR0 host aperture: a raw linear TTM mapping can alias an
 * unrelated native surface on P690.  The list owns one GEM reference, and
 * entries remain alive until no submitted engine work can consume them. */
struct parhelia_vertex_bo {
	struct drm_gem_dma_object dma;
	struct list_head link;
	struct drm_file *file;
};

/*
 * Predicted engine state after every primary span published by this driver.
 * submit_lock serializes both readers and writers.  Valid bits are separate
 * because a zero-filled software shadow is not evidence that hardware holds
 * zero after initialization, reset, or a failed command stream.
 */
#define PARHELIA_INDEXED_REGS 256u
struct parhelia_reg_shadow {
	u32 value[PARHELIA_INDEXED_REGS];
	DECLARE_BITMAP(valid, PARHELIA_INDEXED_REGS);
};

/*
 * Default-off submit telemetry. Atomic storage keeps debugfs reads coherent
 * without taking submit_lock or perturbing the producer critical section.
 */
struct parhelia_submit_stats {
	atomic64_t async_flips;
	atomic64_t async_submits;
	atomic64_t slot_waits;
	atomic64_t slot_wait_ns;
	atomic64_t tail_waits;
	atomic64_t tail_wait_ns;
	atomic64_t ring_start_writes;
	atomic64_t ring_end_writes;
	atomic64_t queue_high_water;
	atomic64_t engine_irqs;
	atomic64_t fence_waits;
	atomic64_t fence_wait_ns;
	/* Queue-occupied time: from the later of submission and the previous
	 * retirement to retirement, as observed by the retire path. */
	atomic64_t gpu_busy_ns;
	u64 last_retire_ns;
};

struct parhelia_bo;

/* One entry in the P690's 24-bit class-0 virtual address space.  The drm_mm
 * node includes the vendor-required trailing 256 KiB guard.  Only map_size
 * bytes starting at node.start may be exposed to a CPU or engine client.
 * Entries are kept in ascending node.start order and occupy a dense hardware
 * slot prefix after the permanent slot-zero identity descriptor.
 */
struct parhelia_aperture_mapping {
	struct drm_mm_node node;
	struct list_head link;
	struct parhelia_bo *owner;
	u32 surface;
	u32 ctrl;
	u32 dims;
	u32 map_size;
	u8 slot;
	bool active;
};

/* All driver-created GEM objects use this wrapper.  Most retain the standard
 * raw TTM mapping.  Dumb framebuffers additionally own a long-lived host
 * aperture descriptor which gives their native bytes a CPU-linear alias.
 */
enum parhelia_placement_class {
	PARHELIA_PLACEMENT_LEGACY,
	PARHELIA_PLACEMENT_NATIVE_COLOR,
	PARHELIA_PLACEMENT_NATIVE_DEPTH,
	PARHELIA_PLACEMENT_NATIVE_SCANOUT,
};

struct parhelia_bo {
	struct drm_gem_vram_object gbo;
	struct parhelia_aperture_mapping aperture;
	u32 width;
	u32 height;
	u32 gpu_pitch;
	u32 host_pitch;
	u32 alloc_height;
	u32 aperture_offset;
	size_t host_size;
	u8 layout;
	u8 aperture_slot;
	u8 placement_class;
	bool host_aperture;
	/* Pinned from creation until free. */
	bool resident;
};

static inline struct parhelia_bo *to_parhelia_bo(struct drm_gem_object *gem)
{
	return container_of(drm_gem_vram_of_gem(gem), struct parhelia_bo, gbo);
}

struct parhelia_device {
	struct drm_device drm;
	/* VRAM helper TTM callbacks with the P690 move override */
	struct ttm_device_funcs ttm_funcs;
	int (*ttm_helper_move)(struct ttm_buffer_object *bo, bool evict,
			       struct ttm_operation_ctx *ctx,
			       struct ttm_resource *new_mem,
			       struct ttm_place *hop);
	struct pci_dev *pdev;
	const struct parhelia_chip_info *chip;
	void __iomem *vram;
	void __iomem *mmio;
	resource_size_t vram_size;
	struct parhelia_bios bios;
	struct drm_simple_display_pipe pipe;
	/* the simple pipe's plane helpers plus the asynchronous flip hooks */
	struct drm_plane_helper_funcs plane_helper_funcs;
	struct parhelia_output outputs[PARHELIA_MAX_OUTPUTS];
	u8 num_outputs;
	struct mutex ddc_lock;
	/* Slots 1..19 and their BAR0 ranges are global hardware state. */
	struct mutex aperture_lock;
	struct drm_mm aperture_mm;
	struct list_head aperture_mappings;
	u8 aperture_mapping_count;
	bool aperture_live;
	/* Default-off root corruption tracer.  The saved sample survives across
	 * ioctl and mmap boundaries, so a userspace store between two kernel
	 * entries is reported at the next boundary. */
	struct mutex root_trace_lock;
	u32 root_trace_value[3];
	u64 root_trace_seq;
	bool root_trace_valid;
	struct mutex submit_lock;
	struct list_head vertex_bos;
	/* The primary ring is small. Secondary packets use independently allocated
	 * 16 KiB coherent blocks so probe works on fragmented 32-bit DMA memory. */
	void *primary_cpu;
	dma_addr_t primary_bus;
	void *vertex_cpu;
	dma_addr_t vertex_bus;
	void *writeback_cpu;
	dma_addr_t writeback_bus;
	struct parhelia_dma_block dma_blocks[PARHELIA_CALL_CAPACITY];
	/* The synchronous diagnostic path above retains its original allocations.
	 * Normal asynchronous renders use the isolated storage below, so no
	 * returned ioctl can expose memory still consumed by the engine. */
	void *async_primary_cpu;
	dma_addr_t async_primary_bus;
	struct parhelia_submit_slot slots[PARHELIA_ASYNC_SLOTS];
	wait_queue_head_t submit_waitq;
	struct delayed_work retire_work;
	u32 primary_put;
	u32 primary_get;
	u32 queued_slots;
	/* The P690's proven 32 MiB native-target placements provide three live
	 * anchors.  Drain at DRM-file handoff so TTM can evict the old client's
	 * unpinned targets before the next client consumes those anchors. */
	struct drm_file *queue_owner;
	struct drm_file *queue_next;
	struct list_head queue_waiters;
	unsigned long queue_owner_deadline;
	bool queue_handoff;
	u32 next_fence;
	u32 first_fence;
	/* Last FENCE_DONE value and when it advanced, for hang detection. */
	u32 progress_fence;
	unsigned long progress_jiffies;
	bool primary_armed;
	u64 fence_context;
	u32 next_dma_tag;
	struct parhelia_submit_stats submit_stats;
	struct parhelia_reg_shadow reg_shadow;
	u64 reg_shadow_submits;
	u64 reg_shadow_packets;
	u64 reg_shadow_packets_omitted;
	u64 reg_shadow_writes_omitted;
	u64 reg_shadow_invalidations;
	void *warp_cpu;
	dma_addr_t warp_bus;
	/* Second instruction heap holding validated userspace WARP programs.
	 * Instruction fetch is a class-3 bus master read, so a second coherent
	 * allocation is as good an instruction source as the resident one. */
	void *warp_user_cpu;
	dma_addr_t warp_user_bus;
	struct mutex warp_user_lock;
	struct drm_file *warp_user_owner[PARHELIA_WARP_USER_SLOTS];
	u8 warp_user_image[PARHELIA_WARP_USER_SLOTS];
	bool engine_initialized;
	bool raw_aperture_owned;
	bool wedged;
	bool irq_installed;
	u32 last_clock_khz;
	/* Retain all batch references after a timeout until device teardown. */
	struct drm_gem_object **quarantine_objects;
	struct drm_gem_vram_object **quarantine_pinned;
	/* Private mode-7 index snapshot retained when a batch times out. */
	struct drm_gem_vram_object *quarantine_mode7_index;
	u32 quarantine_count;
};

static inline u32 parhelia_warp_entry(struct parhelia_device *pdev, u32 offset)
{
	/* warp_bus is aligned to384 bytes, hence divisible by12. */
	return ((u32)(pdev->warp_bus / 12) * 4 + offset) | 3u;
}

static inline struct parhelia_device *to_parhelia(struct drm_device *drm)
{
	return container_of(drm, struct parhelia_device, drm);
}

static inline u32 parhelia_read(struct parhelia_device *pdev, u32 reg)
{
	return readl(pdev->mmio + reg);
}

static inline void parhelia_write(struct parhelia_device *pdev, u32 reg, u32 val)
{
	/* 0x384 is the documented CPU-side DMA-pad workaround. */
	writel(0, pdev->mmio + 0x0384);
	writel(val, pdev->mmio + reg);
}

static inline void parhelia_write8(struct parhelia_device *pdev, u32 reg, u8 val)
{
	/* The LUT gate is byte-addressed.  Do not turn this into a writel(): its
	 * address is deliberately unaligned and the adjacent bytes are unrelated
	 * display-controller state. */
	writeb(val, pdev->mmio + reg);
}

static inline u8 parhelia_read8(struct parhelia_device *pdev, u32 reg)
{
	return readb(pdev->mmio + reg);
}

int parhelia_modeset_init(struct parhelia_device *pdev);
void parhelia_bios_init(struct parhelia_device *pdev);
bool parhelia_bios_parse(const u8 *rom, size_t size,
			 struct parhelia_bios *bios);
bool parhelia_bios_ddc_pins(const struct parhelia_bios *bios,
			    unsigned int line, u8 *scl, u8 *sda);
int parhelia_ddc_init(struct parhelia_output *output);
irqreturn_t parhelia_irq_handler(int irq, void *arg);
bool parhelia_engine_irq_enabled(void);
int parhelia_get_param_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file);
int parhelia_create_render_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file);
int parhelia_create_vertex_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file);
int parhelia_submit_ioctl(struct drm_device *drm, void *data,
			  struct drm_file *file);
int parhelia_wait_fence_ioctl(struct drm_device *drm, void *data,
				     struct drm_file *file);
int parhelia_get_bo_info_ioctl(struct drm_device *drm, void *data,
			       struct drm_file *file);
int parhelia_upload_render_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file);
int parhelia_download_render_ioctl(struct drm_device *drm, void *data,
				   struct drm_file *file);
int parhelia_submit_idle_lock(struct parhelia_device *pdev);
void parhelia_submit_debugfs_init(struct drm_minor *minor);
struct drm_gem_object *parhelia_gem_create_object(struct drm_device *drm, size_t size);
int parhelia_dumb_create(struct drm_file *file, struct drm_device *drm,
			 struct drm_mode_create_dumb *args);
void parhelia_bo_set_layout(struct drm_gem_vram_object *gbo, u8 layout,
			    u32 width, u32 height, u32 gpu_pitch);
void parhelia_bo_set_placement_class(struct drm_gem_vram_object *gbo, u8 class);
void parhelia_bo_make_unmappable(struct drm_gem_vram_object *gbo);
void parhelia_bo_force_unmappable(struct drm_gem_vram_object *gbo);
bool parhelia_texture_upload_enabled(void);
bool parhelia_bo_is_native(const struct parhelia_bo *bo);
bool parhelia_gem_is_owned(const struct drm_gem_object *gem);
void parhelia_root_trace_check(struct parhelia_device *pdev,
			       const char *event, u64 arg0, u64 arg1, u64 arg2);
void parhelia_host_aperture_fini(struct parhelia_device *pdev);
int parhelia_host_aperture_reserve_bootstrap(struct parhelia_device *pdev);
void parhelia_host_aperture_init(struct parhelia_device *pdev);
int parhelia_host_aperture_acquire(struct parhelia_device *pdev,
				   struct parhelia_aperture_mapping *mapping,
				   u32 map_size, u32 surface, u32 ctrl,
				   u32 dims, struct parhelia_bo *owner);
int parhelia_host_aperture_retarget(struct parhelia_device *pdev,
				    struct parhelia_aperture_mapping *mapping,
				    u32 surface, u32 ctrl, u32 dims);
void parhelia_host_aperture_release(struct parhelia_device *pdev,
				    struct parhelia_aperture_mapping *mapping);
int parhelia_engine_init(struct parhelia_device *pdev);
int parhelia_warp_init(struct parhelia_device *pdev);
void parhelia_warp_trace_vertex(struct parhelia_device *pdev, const char *phase);
int parhelia_warp_program_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file);
void parhelia_warp_postclose(struct parhelia_device *pdev, struct drm_file *file);
bool parhelia_warp_entry_allowed(struct parhelia_device *pdev,
				 struct drm_file *file, u32 entry);
void parhelia_submit_fini(struct parhelia_device *pdev);
int parhelia_submit_init(struct parhelia_device *pdev);
int parhelia_vram_pin(struct drm_gem_vram_object *bo);
void parhelia_vram_unpin(struct drm_gem_vram_object *bo);
int parhelia_vram_bar_addr(struct parhelia_device *pdev,
			   struct drm_gem_vram_object *bo, u64 offset,
			   u64 length, u32 *bar_addr);
int parhelia_vram_clear(struct parhelia_device *pdev,
			struct drm_gem_vram_object *gbo);
int parhelia_vram_gpu_addr(struct parhelia_device *pdev,
			   struct drm_gem_vram_object *bo, u64 offset,
			   u64 length, u32 *gpu_addr);
int parhelia_cursor_init(struct parhelia_device *pdev);
void parhelia_state_shadow_invalidate(struct parhelia_device *pdev,
				      const char *reason);
void parhelia_submit_postclose(struct parhelia_device *pdev,
				   struct drm_file *file);

#endif
