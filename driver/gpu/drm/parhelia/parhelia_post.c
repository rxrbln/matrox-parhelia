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
	if (parhelia_post_state(pdev) != PARHELIA_POST_DONE) {
		dev_err(&pdev->pdev->dev,
			"clocks still read cold after bring-up (%#010x)\n",
			parhelia_read(pdev, PARHELIA_CLOCK_SELECT));
		return -EIO;
	}
	dev_info(&pdev->pdev->dev,
		 "cold bring-up completed: clock select %#010x\n",
		 parhelia_read(pdev, PARHELIA_CLOCK_SELECT));
	return 0;
}
