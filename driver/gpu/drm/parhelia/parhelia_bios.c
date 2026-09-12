// SPDX-License-Identifier: GPL-2.0-only
/* Matrox Parhelia VBIOS PInS output-wiring parser. */
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/unaligned.h>

#include "parhelia_drv.h"

#define PARHELIA_PINS_SIGNATURE		0x412e
#define PARHELIA_PINS_POINTER		0x7ffc
#define PARHELIA_PINS_LENGTH		0x0002
#define PARHELIA_PINS_VERSION		0x0004
#define PARHELIA_PINS_DDC_IDS		0x01b4
#define PARHELIA_PINS_DDC_IDS_END	0x01b8
#define PARHELIA_PINS_MIN_VERSION	0x0600
#define PARHELIA_PINS_NO_CHECKSUM	0x0800
#define PARHELIA_PINS_DDC_IDS_VERSION	0x0609
#define PARHELIA_DDC_DEFAULT_IDS		0x05070608

static bool parhelia_pins_valid_at(const u8 *rom, size_t size, size_t base)
{
	size_t length;
	size_t required;
	u8 sum = 0;
	u16 version;
	size_t i;

	if (base >= size || size - base < PARHELIA_PINS_VERSION + sizeof(u16))
		return false;
	if (get_unaligned_le16(rom + base) != PARHELIA_PINS_SIGNATURE)
		return false;

	version = get_unaligned_le16(rom + base + PARHELIA_PINS_VERSION);
	if (version < PARHELIA_PINS_MIN_VERSION)
		return false;

	length = get_unaligned_le16(rom + base + PARHELIA_PINS_LENGTH);
	required = version > PARHELIA_PINS_DDC_IDS_VERSION ?
		PARHELIA_PINS_DDC_IDS_END : PARHELIA_PINS_VERSION + sizeof(u16);
	if (length < required || length > size - base)
		return false;

	/* The vendor deliberately does not checksum PInS 8.0 and later. */
	if (version >= PARHELIA_PINS_NO_CHECKSUM)
		return true;

	for (i = 0; i < length; i++)
		sum += rom[base + i];

	return !sum;
}

/*
 * Parse only the board data needed for native DDC.  The fixed pointer is the
 * authoritative location, with a signature scan retained for ROMs whose
 * pointer was lost while being shadowed.  In particular, the DDC resolver does
 * not consume PInS +0x1a8; disassembly shows fixed candidates for each line.
 */
bool parhelia_bios_parse(const u8 *rom, size_t size,
			 struct parhelia_bios *bios)
{
	size_t base = 0;
	u16 version;
	size_t i;

	if (!bios)
		return false;

	memset(bios, 0, sizeof(*bios));
	bios->ddc_pin_ids = PARHELIA_DDC_DEFAULT_IDS;

	if (!rom || size < PARHELIA_PINS_POINTER + sizeof(u16) ||
	    rom[0] != 0x55 || rom[1] != 0xaa)
		return false;

	base = get_unaligned_le16(rom + PARHELIA_PINS_POINTER);
	if (!parhelia_pins_valid_at(rom, size, base)) {
		base = 0;
		for (i = 0; i < size; i++) {
			if (size - i < PARHELIA_PINS_VERSION + sizeof(u16))
				break;
			if (get_unaligned_le16(rom + i) ==
					PARHELIA_PINS_SIGNATURE &&
			    parhelia_pins_valid_at(rom, size, i)) {
				base = i;
				break;
			}
		}
		if (!base)
			return false;
	}

	version = get_unaligned_le16(rom + base + PARHELIA_PINS_VERSION);
	bios->pins_valid = true;
	bios->pins_version = version;
	bios->pins_offset = base;
	if (version > PARHELIA_PINS_DDC_IDS_VERSION) {
		bios->ddc_pin_ids =
			get_unaligned_le32(rom + base + PARHELIA_PINS_DDC_IDS);
		bios->ddc_pin_ids_from_rom = true;
	}

	return true;
}

static int parhelia_ddc_id_to_pin(u32 ids, u8 id)
{
	unsigned int pin;

	for (pin = 0; pin < 4; pin++)
		if (((ids >> (8 * pin)) & 0xff) == id)
			return pin;

	return -ENOENT;
}

/*
 * Resolve the fixed logical DDC-line candidates through the per-board pin-id
 * table.  line 0 is DVI-1 and line 1 is DVI-2 on the known boards.  These
 * candidates reproduce both original Parhelia ({8,6,7,5}) and P690
 * ({2,1,4,3}) wiring without relying on the unrelated PInS +0x1a8 word.
 */
bool parhelia_bios_ddc_pins(const struct parhelia_bios *bios,
			    unsigned int line, u8 *scl, u8 *sda)
{
	static const u8 candidates[2][4][2] = {
		{ { 1, 2 }, { 5, 6 }, { 0xd, 0xe }, { 9, 0xa } },
		{ { 3, 4 }, { 7, 8 }, { 0xd, 0xe }, { 0xb, 0xc } },
	};
	unsigned int i;

	if (!bios || !scl || !sda || line >= ARRAY_SIZE(candidates))
		return false;

	for (i = 0; i < ARRAY_SIZE(candidates[line]); i++) {
		int scl_pin = parhelia_ddc_id_to_pin(bios->ddc_pin_ids,
						       candidates[line][i][0]);
		int sda_pin = parhelia_ddc_id_to_pin(bios->ddc_pin_ids,
						       candidates[line][i][1]);

		if (scl_pin >= 0 && sda_pin >= 0) {
			*scl = scl_pin;
			*sda = sda_pin;
			return true;
		}
	}

	return false;
}

void parhelia_bios_init(struct parhelia_device *pdev)
{
	void __iomem *rom;
	struct parhelia_bios parsed;
	size_t size;
	u8 *copy;

	memset(&pdev->bios, 0, sizeof(pdev->bios));
	pdev->bios.ddc_pin_ids = PARHELIA_DDC_DEFAULT_IDS;

	rom = pci_map_rom(pdev->pdev, &size);
	if (!rom) {
		dev_info(&pdev->pdev->dev,
			 "VBIOS unavailable; using default DDC pin table %#010x\n",
			 pdev->bios.ddc_pin_ids);
		return;
	}

	copy = kmalloc(size, GFP_KERNEL);
	if (copy)
		memcpy_fromio(copy, rom, size);
	pci_unmap_rom(pdev->pdev, rom);
	if (!copy) {
		dev_warn(&pdev->pdev->dev,
			 "cannot copy VBIOS; using default DDC pin table %#010x\n",
			 pdev->bios.ddc_pin_ids);
		return;
	}

	if (parhelia_bios_parse(copy, size, &parsed)) {
		size_t pins_len;

		pdev->bios = parsed;
		/* Cold bring-up needs the PInS clock and memory records long
		 * after the option ROM is unmapped, so keep the structure
		 * itself - sized by its own length word - and not the ROM. */
		pins_len = get_unaligned_le16(copy + parsed.pins_offset +
					      PARHELIA_PINS_LENGTH);
		if (pins_len > size - parsed.pins_offset)
			pins_len = size - parsed.pins_offset;
		pdev->bios.pins = devm_kmemdup(&pdev->pdev->dev,
					       copy + parsed.pins_offset,
					       pins_len, GFP_KERNEL);
		pdev->bios.pins_len = pdev->bios.pins ? pins_len : 0;
		dev_info(&pdev->pdev->dev,
			 "PInS %#x at %#x: %zu bytes retained, DDC pin table %#010x (%s)\n",
			 pdev->bios.pins_version, pdev->bios.pins_offset,
			 pdev->bios.pins_len, pdev->bios.ddc_pin_ids,
			 pdev->bios.ddc_pin_ids_from_rom ? "VBIOS" : "default");
	} else {
		dev_info(&pdev->pdev->dev,
			 "valid PInS unavailable; using default DDC pin table %#010x\n",
			 pdev->bios.ddc_pin_ids);
	}
	kfree(copy);
}
