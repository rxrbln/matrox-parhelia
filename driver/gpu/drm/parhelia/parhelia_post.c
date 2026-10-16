// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Cold bring-up of a Parhelia-512/8X whose firmware never POSTed it: the chip
 * reset, the two clock synthesisers and the four 64-bit memory controllers.
 *
 * Every register, constant and ordering constraint here was recovered from the
 * vendor driver and the board's own VBIOS; the addresses are in
 * xf86-video-parhelia/docs/parhelia-cold-post-bringup-re.md, whose section
 * numbers this file cites.  The vendor's HWDelay(n) is 3n dummy register
 * reads, about n microseconds on a 66 MHz bus (§1.1), so it becomes udelay(n)
 * with the same n, and a posted-write flush read is kept where its loop
 * provided one.
 *
 * Nothing here touches BAR0: on an un-POSTed board a single dword written to
 * the frame-buffer aperture resets the machine.
 */
#include <linux/delay.h>
#include <linux/pci.h>

#include "parhelia_drv.h"

/* §4.1: the six frequency synthesisers, by index. */
static const u32 parhelia_fs_reg[] = {
	0x0e10, 0x0e14, 0x0e18, 0x0e1c, 0x0e20, 0x0e2c,
};

#define PARHELIA_FS_STROBE		BIT(31)
#define PARHELIA_FS_LOCK_MASK		(BIT(24) | BIT(25))
/* Bits 22, 23 and 29 are never written by either vendor writer (§4.4). */
#define PARHELIA_FS_PRESERVE		(BIT(22) | BIT(23) | BIT(29))

#define PARHELIA_CLOCK_SELECT		0x0e00
#define PARHELIA_CLOCK_OSC_OFF		BIT(30)
#define PARHELIA_CLOCK_PARK		0x8081

#define PARHELIA_MC_SELECT		0x0f00
#define PARHELIA_MC_CONFIG		0x0f08
#define PARHELIA_MC_READY		BIT(31)
#define PARHELIA_MC_SEQUENCER		0x0f5c
#define PARHELIA_MC_STATUS		0x0f24
#define PARHELIA_MC_PHASE_RESET		BIT(16)

#define PARHELIA_PINS_PLL_RECORD(i)	(0x24 + 0x14 * (i))
#define PARHELIA_PINS_CLOCK_SELECT	0x84
#define PARHELIA_PINS_MEMORY_FLAGS	0x8c
#define PARHELIA_PINS_MEMORY_HALF	BIT(3)

#define PARHELIA_FS_REFERENCE_KHZ	27000u

/* §5.3: the 13 dwords of a PInS memory record, in register order. */
static const u32 parhelia_mc_record_reg[] = {
	0x0f08, 0x0f04, 0x0f0c, 0x0f20, 0x0f14, 0x0f18, 0x0f1c,
	0x0f10, 0x0f48, 0x0f4c, 0x0f54, 0x0f58, 0x0f50,
};

/* §5.2: bank mask, PInS record offset and the order they are programmed in. */
static const struct {
	u32 mask;
	unsigned int record;
} parhelia_mc_banks[] = {
	{ 0x00000100, 0x0d8 },	/* bank 1 first */
	{ 0x00000001, 0x0a4 },
	{ 0x00010000, 0x10c },
	{ 0x01000000, 0x140 },
};

/* A dummy read of the interrupt-status register, the flush the vendor's delay
 * loop provided. */
static void parhelia_post_flush(struct parhelia_device *pdev)
{
	parhelia_read(pdev, PARHELIA_MMIO_IRQ_STATUS);
}

static void parhelia_post_delay(struct parhelia_device *pdev, unsigned int usec)
{
	parhelia_post_flush(pdev);
	if (usec >= 1000)
		usleep_range(usec, usec + usec / 8);
	else
		udelay(usec);
}

/* §4.2: true when any of the three clock domains sources synthesiser @index.
 * Domain 0 names it as 1..4 in bits 3:1; domains 1 and 2 accept the pair
 * {2i+1, 2i+2} in bits 11:8 and 19:16. */
static bool parhelia_fs_in_use(u32 select, unsigned int index)
{
	u32 domain0 = (select >> 1) & 7;
	u32 domain1 = (select >> 8) & 0xf;
	u32 domain2 = (select >> 16) & 0xf;

	return domain0 == index + 1 ||
	       domain1 == 2 * index + 1 || domain1 == 2 * index + 2 ||
	       domain2 == 2 * index + 1 || domain2 == 2 * index + 2;
}

/* §4.4: the four negated-count fields. */
static u32 parhelia_fs_encode(u32 a, u32 b, u32 c, u32 d)
{
	u32 word = ((a - 1) & 0x20) | ((-a) & 0x1f);

	word |= ((-b) & 0x1f) << 6;
	word |= ((-c) & 0x1f) << 11;
	/* Bit 16 alone is the bypass code, which is what a divide by two is. */
	word |= (d == 2 ? 1u : ((-d) & 0x3e)) << 16;
	return word;
}

static u32 parhelia_round_div(u32 n, u32 d)
{
	return d ? (n + d / 2) / d : 0;
}

/* §4.4b mtx400: the VCO1 multiplier and stage-1 divider that land closest to a
 * wanted stage-1 output.  A scans downwards and 33 is blacklisted everywhere. */
static bool parhelia_fs_stage1(u32 want_khz, const struct parhelia_fs_limits *lim,
			       u32 *out_a, u32 *out_c)
{
	u32 best = U32_MAX, a;

	if (!want_khz)
		return false;
	for (a = 64; a >= 2; a--) {
		u32 vco1 = PARHELIA_FS_REFERENCE_KHZ * a;
		u32 c, err;

		if (a == 33 || vco1 < lim->vco_min || vco1 > lim->vco_max)
			continue;
		c = parhelia_round_div(vco1, want_khz);
		if (c < 1 || c > 32)
			continue;
		err = abs_diff(vco1 / c, want_khz);
		if (err >= best)
			continue;
		best = err;
		*out_a = a;
		*out_c = c;
	}
	return best != U32_MAX;
}

/* §4.4b mtx402: B descending outside D ascending, keeping the best candidate.
 * This is the search that reaches 550000 kHz exactly, where the ROM's
 * first-fit does not. */
bool parhelia_fs_solve(u32 target_khz, const struct parhelia_fs_limits *lim,
		       u32 *word, u32 *achieved_khz)
{
	u32 best = U32_MAX, b;

	for (b = 32; b >= 2; b--) {
		u32 d;

		for (d = 2; d <= 64; d += 2) {
			u32 want = (u32)div_u64((u64)target_khz * d, b);
			u32 a, c, vco2, out, err;

			if (want < lim->out_min || want > lim->out_max)
				continue;
			if (!parhelia_fs_stage1(want, lim, &a, &c))
				continue;
			vco2 = PARHELIA_FS_REFERENCE_KHZ * a * b / c;
			if (vco2 < lim->vco_min || vco2 > lim->vco_max)
				continue;
			out = (PARHELIA_FS_REFERENCE_KHZ * a / c) * b / d;
			err = abs_diff(out, target_khz);
			if (err >= best)
				continue;
			best = err;
			*word = parhelia_fs_encode(a, b, c, d);
			*achieved_khz = out;
		}
	}
	return best != U32_MAX;
}

/* PInS PLL record @index: the six limit words, megahertz in the record. */
bool parhelia_fs_pins_limits(const struct parhelia_bios *bios,
			     unsigned int index, struct parhelia_fs_limits *lim)
{
	unsigned int record = PARHELIA_PINS_PLL_RECORD(index);

	if (!parhelia_pins_has(bios, record, 0x14))
		return false;
	lim->vco_min = parhelia_pins_u16(bios, record + 0x08) * 1000u;
	lim->vco_max = parhelia_pins_u16(bios, record + 0x0a) * 1000u;
	lim->vco2_min = parhelia_pins_u16(bios, record + 0x0c) * 1000u;
	lim->vco2_max = parhelia_pins_u16(bios, record + 0x0e) * 1000u;
	lim->out_min = parhelia_pins_u16(bios, record + 0x10) * 1000u;
	lim->out_max = parhelia_pins_u16(bios, record + 0x12) * 1000u;
	return lim->vco_min && lim->vco_max && lim->out_min && lim->out_max &&
	       lim->vco2_min && lim->vco2_max;
}

/* §4.4a, the VBIOS SetPixelClock search: A, B, C, D first fit within 0.51 %.
 * Returns the @skip'th acceptable candidate, so a caller can step past words
 * that do not lock exactly as the ROM does. */
bool parhelia_fs_rom_solve(u32 target_khz, const struct parhelia_fs_limits *lim,
			   unsigned int skip, u32 *word, u32 *achieved_khz)
{
	const u32 ref = PARHELIA_FS_REFERENCE_KHZ;
	u32 a_min = lim->vco_min / ref, a_max = lim->vco_max / ref;
	u32 c_min = max(3u, a_min * ref / lim->out_max);
	u32 c_max = a_max * ref / lim->out_min;
	u32 b_min = max_t(u32, 3u, div_u64((u64)lim->vco2_max * c_min, a_max * ref));
	u32 b_max = div_u64((u64)lim->vco2_min * c_max, a_min * ref);
	u32 a, b, c, d;

	if (!target_khz || !a_min || !c_max)
		return false;
	for (a = a_min; a <= a_max; a++)
		for (b = b_min; b <= b_max; b++)
			for (c = c_min; c <= c_max; c++)
				for (d = 0; d <= 0x40; d += 2) {
					u64 vco2 = (u64)ref * a * b;
					u32 out = div_u64(vco2, c) / (d ? d : 2);

					if (div_u64((u64)abs_diff(out, target_khz) * 10000,
						    target_khz) >= 0x33)
						continue;
					if (a == 33 || d == 2)
						continue;
					if (ref * a < lim->vco_min ||
					    ref * a > lim->vco_max ||
					    ref * a / c < lim->out_min ||
					    ref * a / c > lim->out_max ||
					    div_u64(vco2, c) < lim->vco2_min ||
					    div_u64(vco2, c) > lim->vco2_max)
						continue;
					if (skip--)
						continue;
					/* D == 0 is the ROM's spelling of the /2 bypass. */
					*word = parhelia_fs_encode(a, b, c, d ? d : 2);
					*achieved_khz = out;
					return true;
				}
	return false;
}

/* §4.5: both lock bits, sampled 100 times after a fixed settle.  The primary
 * vendor path demands all 100; its fallback accepts 90. */
bool parhelia_fs_locked(struct parhelia_device *pdev, u32 reg,
			unsigned int required)
{
	unsigned int hits = 0, i;

	udelay(201);
	for (i = 0; i < 100; i++)
		if ((parhelia_read(pdev, reg) & PARHELIA_FS_LOCK_MASK) ==
		    PARHELIA_FS_LOCK_MASK)
			hits++;
	return hits >= required;
}

/* §4.4b pllHwInit: load the divider fields with the strobe asserted, release
 * it, then require both VCOs locked.  Bits 26 and 27 are cleared, 28 and 30
 * are zero on this family, and 22, 23 and 29 are preserved. */
static int parhelia_fs_load(struct parhelia_device *pdev, u32 reg, u32 word)
{
	u32 v = parhelia_read(pdev, reg);

	v &= PARHELIA_FS_PRESERVE;
	v |= word | PARHELIA_FS_STROBE;
	parhelia_write(pdev, reg, v);
	parhelia_write(pdev, reg, v & ~PARHELIA_FS_STROBE);
	parhelia_post_delay(pdev, 10);
	if (!parhelia_fs_locked(pdev, reg, 100))
		return -ETIMEDOUT;
	return 0;
}

/* §4.3, §5.2: the engine and both display controllers are held across a clock
 * or memory-controller change. */
struct parhelia_post_hold {
	u32 engine_a, engine_b, head_a, head_b;
};

static void parhelia_post_hold_engine(struct parhelia_device *pdev,
				      struct parhelia_post_hold *hold)
{
	hold->engine_a = parhelia_read(pdev, 0x1c00);
	hold->engine_b = parhelia_read(pdev, 0x1c04);
	hold->head_a = parhelia_read(pdev, 0x1800);
	hold->head_b = parhelia_read(pdev, 0x1a00);
	parhelia_write(pdev, 0x1c00, hold->engine_a | 0x20);
	parhelia_write(pdev, 0x1c04, hold->engine_b | 0x20);
	parhelia_write(pdev, 0x1800, hold->head_a & ~1u);
	parhelia_write(pdev, 0x1a00, hold->head_b & ~1u);
	parhelia_post_delay(pdev, 1000);
}

static void parhelia_post_release_engine(struct parhelia_device *pdev,
					 const struct parhelia_post_hold *hold)
{
	parhelia_write(pdev, 0x1c00, hold->engine_a);
	parhelia_write(pdev, 0x1c04, hold->engine_b);
	parhelia_write(pdev, 0x1800, hold->head_a);
	parhelia_write(pdev, 0x1a00, hold->head_b);
}

/* §2: classify the chip from reads alone. */
enum parhelia_post_state parhelia_post_state(struct parhelia_device *pdev)
{
	u32 select = parhelia_read(pdev, PARHELIA_CLOCK_SELECT);
	u32 expect = parhelia_pins_u32(&pdev->bios, PARHELIA_PINS_CLOCK_SELECT);
	bool synthesiser_dead = true;
	unsigned int i;

	/* §2.1, the vendor's own liveness test: bit 30 is the power-on state
	 * of the reference oscillator power-down, and clearing it is the first
	 * thing the VBIOS does. */
	if (!select || select == PARHELIA_CLOCK_OSC_OFF)
		return PARHELIA_POST_COLD;

	/* §2.2: a POSTed chip has programmed the synthesisers its select word
	 * names; every register reading 0 or a bare load strobe means none of
	 * them was ever loaded. */
	for (i = 0; i < ARRAY_SIZE(parhelia_fs_reg); i++) {
		u32 v = parhelia_read(pdev, parhelia_fs_reg[i]);

		if (v && v != PARHELIA_FS_STROBE)
			synthesiser_dead = false;
	}
	if (synthesiser_dead)
		return PARHELIA_POST_COLD;

	/* §2.3: the 3-D state block returns one constant while unclocked. */
	if (parhelia_read(pdev, 0x1200) == parhelia_read(pdev, 0x12fc) &&
	    parhelia_read(pdev, 0x1200) == parhelia_read(pdev, 0x13fc) &&
	    parhelia_read(pdev, 0x1200))
		return PARHELIA_POST_COLD;

	if (expect && select == expect)
		return PARHELIA_POST_DONE;
	/* Some domain is sourced from a synthesiser rather than the bare
	 * reference clock, which is what a POSTed chip looks like without a
	 * PInS copy to compare against. */
	if ((select >> 1) & 7)
		return PARHELIA_POST_DONE;
	return PARHELIA_POST_UNKNOWN;
}

/* §3.1 HSLPARHardwareReset, with our own COMMAND restored rather than the
 * pre-reset one: firmware left decode off on a board it never POSTed, and a
 * literal transcription would switch it back off mid-bring-up. */
static int parhelia_post_chip_reset(struct parhelia_device *pdev)
{
	struct pci_dev *pci = pdev->pdev;
	u32 saved[7], select300;
	static const int offset[] = { 0x04, 0x0c, 0x10, 0x14, 0x30, 0x3c, 0x40 };
	unsigned int i;
	u16 command;

	pci_read_config_word(pci, PCI_COMMAND, &command);
	for (i = 0; i < ARRAY_SIZE(offset); i++)
		pci_read_config_dword(pci, offset[i], &saved[i]);

	if (pci->device == PARHELIA_8X || pci->device == PARHELIA_LX ||
	    pci->device == PARHELIA_LX2 || pci->device == PARHELIA_P690)
		parhelia_write(pdev, 0x0308, 3);

	/* On a plain PCI board the PCI-X dwords are absent, so the rebuilt
	 * value reduces to the low half plus one bit out of COMMAND. */
	select300 = parhelia_read(pdev, 0x0300) & 0xffffu;
	select300 |= (saved[0] >> 2) & 0x80000u;
	parhelia_write(pdev, 0x0300, select300 & ~0x100u);

	/* PMCSR at 0xe0 on this variant, and no delay between the two halves
	 * of the D3hot to D0 strobe. */
	pci_write_config_dword(pci, 0xe0, 3);
	pci_write_config_dword(pci, 0xe0, 0);
	usleep_range(10000, 11000);

	for (i = 1; i < ARRAY_SIZE(offset); i++)
		pci_write_config_dword(pci, offset[i], saved[i]);
	/* Decode and bus mastering as this driver wants them, not as firmware
	 * left them, and before any further register access. */
	pci_write_config_word(pci, PCI_COMMAND, command | PCI_COMMAND_MEMORY);
	parhelia_write(pdev, 0x0300, select300);
	return 0;
}

/* §4.3 HWPARInitializeSystemClock. */
static int parhelia_post_clock_init(struct parhelia_device *pdev)
{
	u32 select = parhelia_pins_u32(&pdev->bios, PARHELIA_PINS_CLOCK_SELECT);
	u32 previous;
	unsigned int i;
	int ret;

	if (!select) {
		dev_err(&pdev->pdev->dev,
			"PInS carries no clock source selection\n");
		return -ENODATA;
	}
	previous = parhelia_read(pdev, PARHELIA_CLOCK_SELECT);
	/* Park every domain on the reference clock while the synthesisers are
	 * loaded.  0x8081 is the vendor's scratch value; its bits are not
	 * decoded, so it is written as the constant it is. */
	parhelia_write(pdev, PARHELIA_CLOCK_SELECT,
		       previous | PARHELIA_CLOCK_PARK);
	parhelia_write(pdev, PARHELIA_CLOCK_SELECT, PARHELIA_CLOCK_PARK);
	parhelia_write(pdev, PARHELIA_CLOCK_SELECT, 0);
	parhelia_post_delay(pdev, 1000);

	for (i = 0; i < 4; i++) {
		unsigned int record = PARHELIA_PINS_PLL_RECORD(i);
		struct parhelia_fs_limits lim;
		u32 target, divider, word = 0, achieved = 0;

		if (!parhelia_fs_in_use(select, i))
			continue;
		if (!parhelia_pins_has(&pdev->bios, record, 0x14)) {
			dev_err(&pdev->pdev->dev,
				"PInS has no PLL record for synthesiser %u\n", i);
			return -ENODATA;
		}
		target = parhelia_pins_u32(&pdev->bios, record);
		divider = parhelia_pins_u32(&pdev->bios, record + 0x04);
		lim.vco_min = parhelia_pins_u16(&pdev->bios, record + 0x08) * 1000u;
		lim.vco_max = parhelia_pins_u16(&pdev->bios, record + 0x0a) * 1000u;
		lim.out_min = parhelia_pins_u16(&pdev->bios, record + 0x10) * 1000u;
		lim.out_max = parhelia_pins_u16(&pdev->bios, record + 0x12) * 1000u;

		if (divider) {
			/* A board that ships a ready-made divider word takes
			 * the short path: strobe it in and trust it, as the
			 * vendor does. */
			parhelia_write(pdev, parhelia_fs_reg[i],
				       divider | PARHELIA_FS_STROBE);
			parhelia_post_delay(pdev, 100);
			parhelia_write(pdev, parhelia_fs_reg[i], divider);
			continue;
		}
		if (!parhelia_fs_solve(target, &lim, &word, &achieved)) {
			dev_err(&pdev->pdev->dev,
				"no divider solution for synthesiser %u at %u kHz\n",
				i, target);
			return -ERANGE;
		}
		ret = parhelia_fs_load(pdev, parhelia_fs_reg[i], word);
		if (ret) {
			dev_err(&pdev->pdev->dev,
				"synthesiser %u did not lock at %u kHz (word %#010x)\n",
				i, achieved, word);
			return ret;
		}
		dev_info(&pdev->pdev->dev,
			 "synthesiser %u locked at %u kHz for a %u kHz target (word %#010x)\n",
			 i, achieved, target, word);
	}
	usleep_range(10000, 11000);

	parhelia_write(pdev, PARHELIA_CLOCK_SELECT, PARHELIA_CLOCK_PARK);
	parhelia_write(pdev, PARHELIA_CLOCK_SELECT,
		       select | PARHELIA_CLOCK_PARK);
	parhelia_write(pdev, PARHELIA_CLOCK_SELECT, select);
	return 0;
}

/* §5.1: the banked window.  A banked access saves the select register and
 * puts it back if it changed. */
static void parhelia_mc_write(struct parhelia_device *pdev, u32 reg, u32 mask,
			      u32 value)
{
	u32 saved = parhelia_read(pdev, PARHELIA_MC_SELECT);

	parhelia_write(pdev, PARHELIA_MC_SELECT, mask);
	parhelia_write(pdev, reg, value);
	if (parhelia_read(pdev, PARHELIA_MC_SELECT) != saved)
		parhelia_write(pdev, PARHELIA_MC_SELECT, saved);
}

static u32 parhelia_mc_read(struct parhelia_device *pdev, u32 reg, u32 mask)
{
	u32 saved = parhelia_read(pdev, PARHELIA_MC_SELECT);
	u32 value;

	parhelia_write(pdev, PARHELIA_MC_SELECT, mask);
	value = parhelia_read(pdev, reg);
	if (parhelia_read(pdev, PARHELIA_MC_SELECT) != saved)
		parhelia_write(pdev, PARHELIA_MC_SELECT, saved);
	return value;
}

/* §5.1: 0x0f08 bit 31 is a ready flag, not a busy flag - wait for it to be
 * set.  The vendor bound is 100000 iterations with no sleep. */
static int parhelia_mc_wait_ready(struct parhelia_device *pdev, u32 mask)
{
	unsigned int i;

	for (i = 0; i < 100000; i++)
		if (parhelia_mc_read(pdev, PARHELIA_MC_CONFIG, mask) &
		    PARHELIA_MC_READY)
			return 0;
	return -ETIMEDOUT;
}

/* §5.4 HWPARSetMCRegisters: the 27-step per-bank DRAM initialisation.  The
 * order is the actual memory bring-up and none of it is negotiable. */
static int parhelia_mc_program_bank(struct parhelia_device *pdev, u32 mask,
				    const u32 *record)
{
	u32 train = ((record[0] >> 16) & 0xfff) << 16;
	unsigned int i;
	int ret;

	parhelia_write(pdev, PARHELIA_MC_SELECT, mask);
	parhelia_mc_write(pdev, PARHELIA_MC_CONFIG, mask, 0x1004);
	ret = parhelia_mc_wait_ready(pdev, mask);
	if (ret)
		return ret;
	parhelia_mc_write(pdev, PARHELIA_MC_SEQUENCER, mask, 0x138);
	parhelia_mc_write(pdev, 0x0f40, mask, 0);
	parhelia_mc_write(pdev, 0x0f44, mask, 0);
	parhelia_post_delay(pdev, 5000);

	parhelia_mc_write(pdev, 0x0f04, mask, record[1]);
	parhelia_mc_write(pdev, PARHELIA_MC_CONFIG, mask, train | 0x9004);
	for (i = 2; i < ARRAY_SIZE(parhelia_mc_record_reg); i++)
		parhelia_mc_write(pdev, parhelia_mc_record_reg[i], mask,
				  record[i]);

	/* 0x130 launches the training run and 0x100 parks the sequencer; the
	 * wait between them is the training time. */
	parhelia_mc_write(pdev, PARHELIA_MC_SEQUENCER, mask, 0x130);
	parhelia_post_delay(pdev, 21000);
	parhelia_mc_write(pdev, PARHELIA_MC_SEQUENCER, mask, 0x100);
	parhelia_mc_write(pdev, PARHELIA_MC_CONFIG, mask, train | 0x9005);
	parhelia_post_delay(pdev, 1000);
	/* Only this last write installs the board's own configuration. */
	parhelia_mc_write(pdev, PARHELIA_MC_CONFIG, mask, record[0]);
	parhelia_post_delay(pdev, 1000);
	return 0;
}

/* §5.1 HWResetMemoryPhaseError, after every bank and after the holds. */
static void parhelia_mc_phase_reset(struct parhelia_device *pdev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(parhelia_mc_banks); i++) {
		u32 mask = parhelia_mc_banks[i].mask;
		u32 v = parhelia_mc_read(pdev, PARHELIA_MC_CONFIG, mask);

		parhelia_mc_write(pdev, PARHELIA_MC_CONFIG, mask, v | BIT(2));
		parhelia_mc_wait_ready(pdev, mask);
	}
	for (i = 0; i < ARRAY_SIZE(parhelia_mc_banks); i++) {
		u32 mask = parhelia_mc_banks[i].mask;
		u32 v = parhelia_mc_read(pdev, PARHELIA_MC_STATUS, mask);

		parhelia_mc_write(pdev, PARHELIA_MC_STATUS, mask,
				  v | PARHELIA_MC_PHASE_RESET);
		parhelia_post_delay(pdev, 10);
		parhelia_mc_write(pdev, PARHELIA_MC_STATUS, mask,
				  v & ~PARHELIA_MC_PHASE_RESET);
	}
	for (i = 0; i < ARRAY_SIZE(parhelia_mc_banks); i++) {
		u32 mask = parhelia_mc_banks[i].mask;
		u32 v = parhelia_mc_read(pdev, PARHELIA_MC_CONFIG, mask);

		parhelia_mc_write(pdev, PARHELIA_MC_CONFIG, mask, v & ~BIT(2));
	}
	parhelia_post_delay(pdev, 100);
}

/* §2.4 HSLPARMemoryControllerCheck: 0x0f24[1:0] is a per-bank phase error, and
 * any bank reporting one means the controller is not usable. */
static bool parhelia_mc_healthy(struct parhelia_device *pdev, bool half)
{
	unsigned int i, banks = half ? 2 : ARRAY_SIZE(parhelia_mc_banks);
	bool healthy = true;

	for (i = 0; i < banks; i++) {
		u32 mask = parhelia_mc_banks[i].mask;
		u32 status = parhelia_mc_read(pdev, PARHELIA_MC_STATUS, mask) & 3;

		if (status) {
			dev_err(&pdev->pdev->dev,
				"memory bank mask %#010x reports phase error %u\n",
				mask, status);
			healthy = false;
		}
	}
	return healthy;
}

/* §5.2 HWPARInitializeMemoryController: the four banks in the order 1, 0, 2, 3,
 * each from its own PInS record - banks 2 and 3 differ from 0 and 1 in two
 * words, so the records must not be collapsed. */
static int parhelia_post_memory_init(struct parhelia_device *pdev, bool half)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(parhelia_mc_banks); i++) {
		unsigned int at = parhelia_mc_banks[i].record;
		u32 record[ARRAY_SIZE(parhelia_mc_record_reg)];
		unsigned int j;

		/* A half-populated board has only the first two banks. */
		if (half && i >= 2)
			break;
		if (!parhelia_pins_has(&pdev->bios, at, sizeof(record))) {
			dev_err(&pdev->pdev->dev,
				"PInS has no memory record at %#x\n", at);
			return -ENODATA;
		}
		for (j = 0; j < ARRAY_SIZE(record); j++)
			record[j] = parhelia_pins_u32(&pdev->bios,
						      at + 4 * j);
		ret = parhelia_mc_program_bank(pdev,
					       parhelia_mc_banks[i].mask,
					       record);
		if (ret) {
			dev_err(&pdev->pdev->dev,
				"memory bank mask %#010x did not become ready\n",
				parhelia_mc_banks[i].mask);
			return ret;
		}
	}
	parhelia_write(pdev, 0x12b8, 0x00070601);
	parhelia_write(pdev, 0x12b0, 0);
	return 0;
}

/* The whole cold sequence, in vendor order (§6).  The caller must not have
 * touched BAR0 yet. */
int parhelia_post_cold_bringup(struct parhelia_device *pdev)
{
	struct parhelia_post_hold hold;
	bool half = parhelia_pins_u32(&pdev->bios, PARHELIA_PINS_MEMORY_FLAGS) &
		    PARHELIA_PINS_MEMORY_HALF;
	int ret;

	if (pdev->chip->mc_family != PARHELIA_MC_HSLPAR ||
	    pdev->chip->clock_family != PARHELIA_CLOCK_HSLPAR) {
		dev_err(&pdev->pdev->dev,
			"cold bring-up is only recovered for the HSLPAR families\n");
		return -EOPNOTSUPP;
	}
	if (!pdev->bios.pins_len) {
		dev_err(&pdev->pdev->dev,
			"cold bring-up needs the board's PInS structure\n");
		return -ENODATA;
	}

	dev_info(&pdev->pdev->dev,
		 "cold bring-up: resetting the chip and programming %s memory\n",
		 half ? "two banks of" : "four banks of");
	ret = parhelia_post_chip_reset(pdev);
	if (ret)
		return ret;

	/* The clock change and the memory bring-up share one hold. */
	parhelia_post_hold_engine(pdev, &hold);
	ret = parhelia_post_clock_init(pdev);
	if (!ret)
		ret = parhelia_post_memory_init(pdev, half);
	parhelia_post_release_engine(pdev, &hold);
	if (ret)
		return ret;
	parhelia_mc_phase_reset(pdev);

	if (!parhelia_mc_healthy(pdev, half))
		return -EIO;
	/* Only the clock selection can be checked here: the 3-D state block
	 * still reads one constant (0xc0, MEASURED after the reset) until
	 * engine initialisation enables it, so parhelia_post_state()'s
	 * unclocked test does not apply yet. */
	if (parhelia_read(pdev, PARHELIA_CLOCK_SELECT) !=
	    parhelia_pins_u32(&pdev->bios, PARHELIA_PINS_CLOCK_SELECT)) {
		dev_err(&pdev->pdev->dev,
			"clock select reads %#010x after bring-up, PInS names %#010x\n",
			parhelia_read(pdev, PARHELIA_CLOCK_SELECT),
			parhelia_pins_u32(&pdev->bios, PARHELIA_PINS_CLOCK_SELECT));
		return -EIO;
	}
	dev_info(&pdev->pdev->dev,
		 "cold bring-up completed: clock select %#010x\n",
		 parhelia_read(pdev, PARHELIA_CLOCK_SELECT));
	return 0;
}

/* ---- P690 (SUN90): the vendor's chip reset and clock/memory bring-up -----
 *
 * HSLSNXHardwareReset, HSLS90InitMemClk, HWS90InitializeSystemClock and
 * HWS90InitializeMemoryController (mtx_drv.so; the Windows miniport carries
 * the same code), as decoded in xf86-video-parhelia/docs/
 * parhelia-p690-engine-reset-re.md and parhelia-runtime-reset-review.md.
 */
#define PARHELIA_S90_MC_SELECT		0x0ffc
#define PARHELIA_S90_MC_CONTROL		0x0ff4
#define PARHELIA_S90_MC_STATUS		0x0ff8
#define PARHELIA_S90_FS(i)		(0x0e30 + 4 * (i))
#define PARHELIA_S90_OSC		0x0e24
#define PARHELIA_S90_FS_LOCK		BIT(24)
#define PARHELIA_S90_FS_STROBES		(BIT(31) | BIT(21))
#define PARHELIA_S90_REF_KHZ		27000u

/* PInS offset, memory-controller register and bank mask of each of the 44
 * dwords HWS90InitializeMemoryController writes, in its order (the table
 * pinsDatasConvertor; byte-identical in the Windows miniport). */
static const struct {
	u16 pins, reg;
	u8 bank;
} parhelia_s90_mc_table[] = {
	{ 0x210, 0x0f00, 3 }, { 0x214, 0x0f04, 3 },
	{ 0x218, 0x0f08, 3 }, { 0x21c, 0x0f10, 3 },
	{ 0x220, 0x0f1c, 3 }, { 0x224, 0x0f20, 3 },
	{ 0x228, 0x0f24, 3 }, { 0x22c, 0x0f28, 3 },
	{ 0x230, 0x0f2c, 3 }, { 0x234, 0x0f44, 3 },
	{ 0x238, 0x0f94, 3 }, { 0x23c, 0x0f98, 3 },
	{ 0x240, 0x0f9c, 3 }, { 0x244, 0x0fa0, 3 },
	{ 0x248, 0x0fc0, 3 }, { 0x24c, 0x0fc4, 3 },
	{ 0x250, 0x0fc8, 3 }, { 0x254, 0x0fcc, 3 },
	{ 0x258, 0x0ff0, 3 }, { 0x25c, 0x0ff8, 3 },
	{ 0x260, 0x0f14, 1 }, { 0x264, 0x0f18, 1 },
	{ 0x268, 0x0f48, 1 }, { 0x26c, 0x0f4c, 1 },
	{ 0x270, 0x0f58, 1 }, { 0x274, 0x0f5c, 1 },
	{ 0x278, 0x0f68, 1 }, { 0x27c, 0x0f6c, 1 },
	{ 0x280, 0x0f70, 1 }, { 0x284, 0x0f74, 1 },
	{ 0x288, 0x0f8c, 1 }, { 0x28c, 0x0f90, 1 },
	{ 0x290, 0x0f14, 2 }, { 0x294, 0x0f18, 2 },
	{ 0x298, 0x0f48, 2 }, { 0x29c, 0x0f4c, 2 },
	{ 0x2a0, 0x0f58, 2 }, { 0x2a4, 0x0f5c, 2 },
	{ 0x2a8, 0x0f68, 2 }, { 0x2ac, 0x0f6c, 2 },
	{ 0x2b0, 0x0f70, 2 }, { 0x2b4, 0x0f74, 2 },
	{ 0x2b8, 0x0f8c, 2 }, { 0x2bc, 0x0f90, 2 },
};

/* Write/ReadS90MemoryControllerDword: select the bank(s) through 0x0ffc and
 * put the old selection back if the access changed it. */
static void parhelia_s90_mc_write(struct parhelia_device *pdev, u32 reg,
				  u32 bank, u32 value)
{
	u32 saved = parhelia_read(pdev, PARHELIA_S90_MC_SELECT);

	parhelia_write(pdev, PARHELIA_S90_MC_SELECT, bank & 3);
	parhelia_write(pdev, reg, value);
	if (parhelia_read(pdev, PARHELIA_S90_MC_SELECT) != saved)
		parhelia_write(pdev, PARHELIA_S90_MC_SELECT, saved);
}

static u32 parhelia_s90_mc_read(struct parhelia_device *pdev, u32 reg,
				u32 bank)
{
	u32 saved = parhelia_read(pdev, PARHELIA_S90_MC_SELECT);
	u32 value;

	parhelia_write(pdev, PARHELIA_S90_MC_SELECT, bank & 3);
	value = parhelia_read(pdev, reg);
	if (parhelia_read(pdev, PARHELIA_S90_MC_SELECT) != saved)
		parhelia_write(pdev, PARHELIA_S90_MC_SELECT, saved);
	return value;
}

/* WaitForS90MemoryIdle: bank 1's 0x0ff8 has bits 9 and 10 both set while
 * the controller is busy; the vendor bound is 100000 reads. */
static int parhelia_s90_mc_wait_idle(struct parhelia_device *pdev)
{
	u32 saved = parhelia_read(pdev, PARHELIA_S90_MC_SELECT);
	unsigned int i;
	int ret = -ETIMEDOUT;

	for (i = 0; i < 100000; i++)
		if ((parhelia_s90_mc_read(pdev, PARHELIA_S90_MC_STATUS, 1) &
		     0x0600) != 0x0600) {
			ret = 0;
			break;
		}
	parhelia_write(pdev, PARHELIA_S90_MC_SELECT, saved);
	return ret;
}

/* HSLSNXHardwareReset: all 64 configuration dwords saved, 0x0300 rebuilt from
 * cfg 0x84/0x8c, the PM D3hot -> D0 strobe with 10 ms per half, the dwords
 * restored except MGAINDEX/MGADATA (0x44/0x48, an MMIO window), COMMAND
 * last - our own, as for the original family. */
static u32 parhelia_s90_select300(struct parhelia_device *pdev, const u32 *cfg)
{
	u32 v = (parhelia_read(pdev, 0x0300) & 0xf888fff8u) |
		(cfg[0x84 / 4] & 7) |
		((cfg[0x84 / 4] >> 6) & 7) << 16 |
		((cfg[0x84 / 4] >> 9) & 7) << 20 |
		((cfg[0x8c / 4] >> 12) & 7) << 24 | 0x600;

	/* bit 8 on every part but the first SUNEX subvariant */
	if (pdev->pdev->device == PARHELIA_P690)
		v |= 0x100;
	return v;
}

static int parhelia_s90_chip_reset(struct parhelia_device *pdev)
{
	struct pci_dev *pci = pdev->pdev;
	/* a chip whose MMIO reads all ones gets 0x0300 after the reset only */
	bool live = parhelia_read(pdev, 0x0300) != ~0u;
	u32 cfg[64], select300 = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cfg); i++)
		pci_read_config_dword(pci, i * 4, &cfg[i]);
	if (live) {
		select300 = parhelia_s90_select300(pdev, cfg);
		parhelia_write(pdev, 0x0300, select300);
	}

	pci_write_config_dword(pci, 0xe0, 3);
	msleep(10);
	pci_write_config_dword(pci, 0xe0, 0);
	msleep(10);

	for (i = 0; i < ARRAY_SIZE(cfg); i++) {
		if (i * 4 == PCI_COMMAND || i * 4 == 0x44 || i * 4 == 0x48)
			continue;
		pci_write_config_dword(pci, i * 4, cfg[i]);
	}
	pci_write_config_dword(pci, PCI_COMMAND,
			       cfg[PCI_COMMAND / 4] | PCI_COMMAND_MEMORY);
	if (!live)
		select300 = parhelia_s90_select300(pdev, cfg);
	parhelia_write(pdev, 0x0300, select300);
	if (parhelia_read(pdev, 0x0300) == ~0u) {
		dev_err(&pci->dev, "MMIO still reads all ones after the chip reset\n");
		return -EIO;
	}
	return 0;
}

/* IsScUseFS_SUN90: synthesiser @fs feeds a domain whose select field
 * (bits 4:1) is fs + 1, 8 + 2 fs or 9 + 2 fs; domain 0 only counts for
 * FS0. */
static bool parhelia_s90_fs_in_use(const u32 *sel, unsigned int fs)
{
	unsigned int d;

	for (d = fs ? 1 : 0; d < 3; d++) {
		u32 f = (sel[d] >> 1) & 0xf;

		if (f == fs + 1 || f == 8 + 2 * fs || f == 9 + 2 * fs)
			return true;
	}
	return false;
}

/* The SUN90 divider word nearest @target within the PInS windows:
 * VCO1 = 27 MHz * M1, tap = VCO1 / P1, VCO2 = tap * M2, out = VCO2 / 2 P2,
 * fields M1-1 [5:0], M2-1 [10:6], P1-1 [15:11], P2-1 [19:16] (the vendor's
 * format and ranges; its tie-break between equally close words is not
 * decoded, so the first of them is taken). */
static bool parhelia_s90_fs_solve(u32 target, const struct parhelia_fs_limits *lim,
				  u32 *word, u32 *achieved)
{
	const u32 ref = PARHELIA_S90_REF_KHZ;
	u32 best_err = U32_MAX, m1, m2, p1, p2;

	for (m1 = 1; m1 <= 64; m1++) {
		u32 vco1 = ref * m1;

		if (vco1 < lim->vco_min || vco1 > lim->vco_max)
			continue;
		for (p1 = 1; p1 <= 32; p1++) {
			u32 tap = vco1 / p1;

			if (tap < lim->out_min || tap > lim->out_max)
				continue;
			for (m2 = 1; m2 <= 32; m2++) {
				u32 vco2 = div_u64((u64)vco1 * m2, p1);

				if (vco2 < lim->vco2_min || vco2 > lim->vco2_max)
					continue;
				for (p2 = 1; p2 <= 16; p2++) {
					u32 out = vco2 / (2 * p2);
					u32 err = abs_diff(out, target);

					if (err >= best_err)
						continue;
					best_err = err;
					*achieved = out;
					*word = (m1 - 1) | (m2 - 1) << 6 |
						(p1 - 1) << 11 | (p2 - 1) << 16;
				}
			}
		}
	}
	/* within 0.5 %, the bound the pixel-clock search also accepts */
	return best_err != U32_MAX && (u64)best_err * 200 <= target;
}

/* sun90pllHwOscOn: the synthesiser oscillator on 0x0e24, bit 30 on and
 * bit 31 off. */
static void parhelia_s90_osc_on(struct parhelia_device *pdev)
{
	u32 v = parhelia_read(pdev, PARHELIA_S90_OSC);

	if (!(v & BIT(31)) && (v & BIT(30)))
		return;
	v &= ~BIT(31);
	parhelia_write(pdev, PARHELIA_S90_OSC, v);
	parhelia_post_delay(pdev, 1000);
	v |= BIT(30);
	parhelia_write(pdev, PARHELIA_S90_OSC, v);
	parhelia_post_delay(pdev, 1000);
}

/* sun90pllHwInit's two-write strobe, then the lock bit: up to 100 polls,
 * and all of 100 further samples locked. */
static int parhelia_s90_fs_load(struct parhelia_device *pdev, unsigned int fs,
				u32 word)
{
	u32 reg = PARHELIA_S90_FS(fs);
	unsigned int i;

	parhelia_write(pdev, reg, word | PARHELIA_S90_FS_STROBES);
	parhelia_write(pdev, reg, word);
	parhelia_post_delay(pdev, 10);
	for (i = 0; i < 100; i++)
		if (parhelia_read(pdev, reg) & PARHELIA_S90_FS_LOCK)
			break;
	if (i == 100)
		return -ETIMEDOUT;
	for (i = 0; i < 100; i++)
		if (!(parhelia_read(pdev, reg) & PARHELIA_S90_FS_LOCK))
			return -EIO;
	return 0;
}

/* HWS90InitializeSystemClock: three domain selects at 0x0e00/04/08 parked
 * on the reference, each synthesiser in use loaded from its PInS record (a
 * ready divider word is strobed in as given, otherwise solved for the target
 * through HWS90InitialiseFSController), the selects restored, then 0x0e24
 * from PInS. */
static int parhelia_s90_clock_init(struct parhelia_device *pdev)
{
	static const u32 select_reg[3] = { 0x0e00, 0x0e04, 0x0e08 };
	u32 sel[3];
	unsigned int i, fs;
	int ret;

	for (i = 0; i < 3; i++)
		sel[i] = parhelia_pins_u32(&pdev->bios, 0x200 + 4 * i);
	if (!sel[0]) {
		dev_err(&pdev->pdev->dev, "PInS carries no SUN90 clock selection\n");
		return -ENODATA;
	}
	for (i = 0; i < 3; i++)
		parhelia_write(pdev, select_reg[i],
			       parhelia_read(pdev, select_reg[i]) | 1);
	for (i = 0; i < 3; i++)
		parhelia_write(pdev, select_reg[i], 1);
	for (i = 0; i < 3; i++)
		parhelia_write(pdev, select_reg[i], 0);
	parhelia_post_delay(pdev, 1000);

	for (fs = 0; fs < 4; fs++) {
		unsigned int rec = 0x24 + 0x14 * fs;
		struct parhelia_fs_limits lim;
		u32 target, divider, word = 0, achieved = 0;

		if (!parhelia_s90_fs_in_use(sel, fs))
			continue;
		if (!parhelia_pins_has(&pdev->bios, rec, 0x14))
			return -ENODATA;
		target = parhelia_pins_u32(&pdev->bios, rec);
		divider = parhelia_pins_u32(&pdev->bios, rec + 4);
		if (divider) {
			parhelia_write(pdev, PARHELIA_S90_FS(fs), divider | BIT(31));
			parhelia_post_delay(pdev, 100);
			parhelia_write(pdev, PARHELIA_S90_FS(fs), divider);
			continue;
		}
		lim.vco_min = parhelia_pins_u16(&pdev->bios, rec + 0x08) * 1000u;
		lim.vco_max = parhelia_pins_u16(&pdev->bios, rec + 0x0a) * 1000u;
		lim.vco2_min = parhelia_pins_u16(&pdev->bios, rec + 0x0c) * 1000u;
		lim.vco2_max = parhelia_pins_u16(&pdev->bios, rec + 0x0e) * 1000u;
		lim.out_min = parhelia_pins_u16(&pdev->bios, rec + 0x10) * 1000u;
		lim.out_max = parhelia_pins_u16(&pdev->bios, rec + 0x12) * 1000u;
		if (!parhelia_s90_fs_solve(target, &lim, &word, &achieved)) {
			dev_err(&pdev->pdev->dev,
				"no SUN90 divider for synthesiser %u at %u kHz\n",
				fs, target);
			return -ERANGE;
		}
		parhelia_s90_osc_on(pdev);
		ret = parhelia_s90_fs_load(pdev, fs, word);
		if (ret) {
			dev_err(&pdev->pdev->dev,
				"synthesiser %u did not lock at %u kHz (word %#010x)\n",
				fs, achieved, word);
			return ret;
		}
		dev_info(&pdev->pdev->dev,
			 "synthesiser %u locked at %u kHz for a %u kHz target (word %#010x)\n",
			 fs, achieved, target, word);
	}
	parhelia_post_delay(pdev, 10000);

	for (i = 0; i < 3; i++)
		parhelia_write(pdev, select_reg[i], 1);
	for (i = 0; i < 3; i++)
		parhelia_write(pdev, select_reg[i], sel[i] | 1);
	for (i = 0; i < 3; i++)
		parhelia_write(pdev, select_reg[i], sel[i]);
	parhelia_write(pdev, PARHELIA_S90_OSC,
		       parhelia_pins_u32(&pdev->bios, 0x20c));
	return 0;
}

/* HWS90InitializeMemoryController.  The vendor's start word is an
 * uninitialised variable or'ed with 1 in both its builds, so only bit 0
 * (start) is significant. */
static int parhelia_s90_memory_init(struct parhelia_device *pdev)
{
	static const u16 tune_reg[4] = { 0x0f48, 0x0f4c, 0x0f58, 0x0f5c };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(parhelia_s90_mc_table); i++)
		parhelia_s90_mc_write(pdev, parhelia_s90_mc_table[i].reg,
				      parhelia_s90_mc_table[i].bank,
				      parhelia_pins_u32(&pdev->bios,
						parhelia_s90_mc_table[i].pins));
	parhelia_s90_mc_write(pdev, PARHELIA_S90_MC_CONTROL, 3, 1);
	parhelia_post_delay(pdev, 5);
	ret = parhelia_s90_mc_wait_idle(pdev);
	if (ret)
		return ret;
	parhelia_s90_mc_write(pdev, 0x0f00, 3,
			      parhelia_s90_mc_read(pdev, 0x0f00, 1) | BIT(30));
	ret = parhelia_s90_mc_wait_idle(pdev);
	if (ret)
		return ret;
	parhelia_s90_mc_write(pdev, PARHELIA_S90_MC_CONTROL, 3, 0);
	/* the tuning words again, bank 1 from PInS 0x268, bank 2 from 0x298 */
	for (i = 0; i < ARRAY_SIZE(tune_reg); i++)
		parhelia_s90_mc_write(pdev, tune_reg[i], 1,
				      parhelia_pins_u32(&pdev->bios, 0x268 + 4 * i));
	for (i = 0; i < ARRAY_SIZE(tune_reg); i++)
		parhelia_s90_mc_write(pdev, tune_reg[i], 2,
				      parhelia_pins_u32(&pdev->bios, 0x298 + 4 * i));
	parhelia_s90_mc_write(pdev, PARHELIA_S90_MC_CONTROL, 3, 1);
	parhelia_post_delay(pdev, 5);
	return parhelia_s90_mc_wait_idle(pdev);
}

/* The P690 recovery: chip reset, then HSLS90InitMemClk under the engine and
 * display holds.  The caller must not touch BAR0 before it returns. */
int parhelia_post_sun90_bringup(struct parhelia_device *pdev)
{
	struct parhelia_post_hold hold;
	unsigned int i;
	int ret;

	if (pdev->chip->mc_family != PARHELIA_MC_SUN90 ||
	    pdev->chip->clock_family != PARHELIA_CLOCK_SUN90)
		return -EOPNOTSUPP;
	if (!parhelia_pins_has(&pdev->bios, 0x200, 0xc0)) {
		dev_err(&pdev->pdev->dev,
			"SUN90 bring-up needs PInS up to 0x2c0\n");
		return -ENODATA;
	}
	dev_info(&pdev->pdev->dev,
		 "SUN90 bring-up: resetting the chip, the system clock and the memory controller\n");
	ret = parhelia_s90_chip_reset(pdev);
	if (ret)
		return ret;

	parhelia_post_hold_engine(pdev, &hold);
	parhelia_s90_mc_write(pdev, PARHELIA_S90_MC_CONTROL, 3, 0);
	ret = parhelia_s90_clock_init(pdev);
	if (!ret)
		ret = parhelia_s90_memory_init(pdev);
	parhelia_post_release_engine(pdev, &hold);
	if (ret)
		return ret;

	for (i = 0; i < 3; i++) {
		u32 want = parhelia_pins_u32(&pdev->bios, 0x200 + 4 * i);

		if (parhelia_read(pdev, 0x0e00 + 4 * i) != want) {
			dev_err(&pdev->pdev->dev,
				"clock select %u reads %#010x after bring-up, PInS names %#010x\n",
				i, parhelia_read(pdev, 0x0e00 + 4 * i), want);
			return -EIO;
		}
	}
	dev_info(&pdev->pdev->dev, "SUN90 bring-up completed: oscillator %#010x\n",
		 parhelia_read(pdev, PARHELIA_S90_OSC));
	return 0;
}
