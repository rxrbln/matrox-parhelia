/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __PARHELIA_FIRMWARE_H__
#define __PARHELIA_FIRMWARE_H__

/* Complete ELF symbol bodies, not the obsolete warp_programs.h arrays or
 * truncated live slot dumps. Raw little-endian instruction triplets.
 * Heap layout: captured 128-instruction slots, body at96 (stage AB at111).
 * Stage C occupies three slots: the full 278-instruction body plus prefix.
 * CRC identifies the audited image revision; it is NOT authentication.
 * No vendor instruction data is included in the driver sources.
 */
#define PARHELIA_RESIDENT_FIRMWARE(X) \
	X("parhelia/mtx3605.bin",   96, 46, 0x62a32734u) \
	X("parhelia/mtx1739.bin",  352, 41, 0x5c2a1dd5u) \
	X("parhelia/mtx1223.bin",  608, 20, 0x8de4b872u) \
	X("parhelia/mtx3213.bin",  736, 16, 0xaea97508u) \
	X("parhelia/mtx2068.bin",  864, 10, 0x3252007cu) \
	X("parhelia/mtx1824.bin",  992, 17, 0xda39ea35u) \
	X("parhelia/mtx251.bin",  1120, 29, 0x8a68ac58u) \
	X("parhelia/stage-ab.bin",1263, 17, 0xf66967a7u) \
	X("parhelia/stage-c.bin", 1376,280, 0xda0e0355u)

#define PARHELIA_WARP_RESIDENT_END 1656u
#define PARHELIA_WARP_STORE_INSTRUCTIONS 2816u
#define PARHELIA_WARP_KIND6_ENTRY (96u * 4)
#define PARHELIA_WARP_KIND12_ENTRY (864u * 4)
#define PARHELIA_WARP_PUBLISH_ENTRY (992u * 4)
#define PARHELIA_WARP_BASE_ENTRY (1120u * 4)
#define PARHELIA_WARP_STAGE_A_ENTRY (1263u * 4)
#define PARHELIA_WARP_STAGE_B_ENTRY (1267u * 4)
#define PARHELIA_WARP_STAGE_C_ENTRY (1376u * 4)

#endif
