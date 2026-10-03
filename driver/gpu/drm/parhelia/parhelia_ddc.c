// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Native monitor DDC over the Parhelia 0x1c48 open-drain GPIO register. */
#include <linux/module.h>

#include <drm/drm_managed.h>

#include "parhelia_drv.h"

static void parhelia_ddc_set_line(struct parhelia_output *output, u8 pin,
				  int high)
{
	struct parhelia_device *pdev = output->pdev;
	u32 data = BIT(pin);
	u32 oe = BIT(pin + 16);
	u32 value = parhelia_read(pdev, PARHELIA_REG_DDC);

	/* The pin is open drain: clear OE to release it high, set OE while the
	 * data latch is zero to drive it low. */
	value &= ~(data | oe);
	if (!high)
		value |= oe;
	parhelia_write(pdev, PARHELIA_REG_DDC, value);
}

static void parhelia_ddc_setsda(void *data, int state)
{
	struct parhelia_output *output = data;

	parhelia_ddc_set_line(output, output->ddc_sda, state);
}

static void parhelia_ddc_setscl(void *data, int state)
{
	struct parhelia_output *output = data;

	parhelia_ddc_set_line(output, output->ddc_scl, state);
}

static int parhelia_ddc_getsda(void *data)
{
	struct parhelia_output *output = data;

	return !!(parhelia_read(output->pdev, PARHELIA_REG_DDC) &
		  BIT(output->ddc_sda));
}

static int parhelia_ddc_getscl(void *data)
{
	struct parhelia_output *output = data;

	return !!(parhelia_read(output->pdev, PARHELIA_REG_DDC) &
		  BIT(output->ddc_scl));
}

static int parhelia_ddc_pre_xfer(struct i2c_adapter *adapter)
{
	struct parhelia_output *output = i2c_get_adapdata(adapter);

	struct parhelia_device *pdev = output->pdev;

	mutex_lock(&pdev->ddc_lock);
	/* The VBIOS (rom0528 0x4d6a) and HSLPARI2COpenPort drive the channel
	 * select before every transaction; 0x1c4c reads the pins, so bit 8
	 * reads 0 and any read-modify-write elsewhere drops the select. */
	if (pdev->chip->output_family == PARHELIA_OUTPUT_ORIGINAL)
		parhelia_write(pdev, PARHELIA_DDC_CHANNEL,
			       parhelia_read(pdev, PARHELIA_DDC_CHANNEL) |
			       PARHELIA_DDC_CHANNEL_ON);
	return 0;
}

static void parhelia_ddc_post_xfer(struct i2c_adapter *adapter)
{
	struct parhelia_output *output = i2c_get_adapdata(adapter);

	mutex_unlock(&output->pdev->ddc_lock);
}

static void parhelia_ddc_release(struct drm_device *drm, void *res)
{
	struct parhelia_output *output = res;

	i2c_del_adapter(&output->ddc);
}

int parhelia_ddc_init(struct parhelia_output *output)
{
	struct parhelia_device *pdev = output->pdev;
	struct i2c_algo_bit_data *bit = &output->ddc_bit;
	struct i2c_adapter *adapter = &output->ddc;
	u32 value;
	int ret;

	bit->data = output;
	bit->setsda = parhelia_ddc_setsda;
	bit->setscl = parhelia_ddc_setscl;
	bit->getsda = parhelia_ddc_getsda;
	bit->getscl = parhelia_ddc_getscl;
	bit->pre_xfer = parhelia_ddc_pre_xfer;
	bit->post_xfer = parhelia_ddc_post_xfer;
	bit->udelay = 10;
	bit->timeout = usecs_to_jiffies(100000);

	adapter->owner = THIS_MODULE;
	adapter->algo_data = bit;
	adapter->dev.parent = pdev->drm.dev;
	snprintf(adapter->name, sizeof(adapter->name),
		 "Parhelia DDC line %u (GPIO %u/%u)", output->desc->ddc_line,
		 output->ddc_scl, output->ddc_sda);
	i2c_set_adapdata(adapter, output);

	/* Release all four known native DDC pins.  This also recovers from an
	 * interrupted probe which left another connector's line asserted. */
	value = parhelia_read(pdev, PARHELIA_REG_DDC);
	parhelia_write(pdev, PARHELIA_REG_DDC, value & ~GENMASK(19, 16));
	/* The original boards gate the DDC lines behind a channel select that
	 * their own VBIOS drives before every transaction; without it the
	 * clock line of connector 0 reads permanently low and nothing
	 * acknowledges.  The P690's pin table does not declare it, which is
	 * why the same code reads EDID there and not here. */
	if (pdev->chip->output_family == PARHELIA_OUTPUT_ORIGINAL) {
		value = parhelia_read(pdev, PARHELIA_DDC_CHANNEL);
		parhelia_write(pdev, PARHELIA_DDC_CHANNEL,
			       value | PARHELIA_DDC_CHANNEL_ON);
	}

	ret = i2c_bit_add_bus(adapter);
	if (ret)
		return ret;

	return drmm_add_action_or_reset(&pdev->drm, parhelia_ddc_release,
					output);
}
