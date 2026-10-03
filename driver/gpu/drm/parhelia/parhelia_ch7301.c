// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Chrontel CH7301 DVI transmitters on the original Parhelia boards.  The DVO
 * ports feed external CH7301s on the MISC-IO (0x1c4c) I2C bus; the VBIOS
 * (rom0528 0x8508 probe, 0x7461 enable) and the vendor HAL (HSLCHREnable,
 * HSLCHREnableDigitalMode) retune their TMDS PLL for every pixel clock.
 */
#include <linux/delay.h>
#include <linux/module.h>

#include <drm/drm_debugfs.h>
#include <drm/drm_file.h>
#include <drm/drm_managed.h>

#include "parhelia_drv.h"

#define PARHELIA_REG_MISCIO	0x1c4c
#define PARHELIA_MISCIO_OE(pin)	BIT((pin) + 10)

#define CH7301_CM		0x1c
#define CH7301_IDF		0x1f
#define CH7301_TCTL		0x23
#define CH7301_TCTL_PLL_LOCK	BIT(3)
#define CH7301_TPVT		0x35
#define CH7301_TPVT_LOCK	BIT(6)
#define CH7301_GPIO		0x1e
#define CH7301_TSTP		0x48
#define CH7301_TSTP_RESETDB	BIT(3)
#define CH7301_PM		0x49
#define CH7301_PM_PLL		BIT(6)
#define CH7301_VID		0x4a
#define CH7301_TSTAT		0x4d
#define CH7301_TSTAT_LOCKED	BIT(2)
#define CH7301_TUNE		0x70

/* MISC-IO bit-bang port: PInS[0x88] gate bit and the SDA/SCL pin ids that
 * rom0528 0x4c26 resolves through PInS[0x188]. */
struct parhelia_miscio_port {
	u8 gate;
	u8 sda_id;
	u8 scl_id;
};

static const struct parhelia_miscio_port parhelia_miscio_ports[] = {
	{ .gate = BIT(2), .sda_id = 1, .scl_id = 2 },	/* VBIOS port 4 */
	{ .gate = BIT(3), .sda_id = 3, .scl_id = 4 },	/* VBIOS port 5 */
};

struct parhelia_miscio_i2c {
	struct i2c_adapter adap;
	struct i2c_algo_bit_data bit;
	struct parhelia_device *pdev;
	u8 sda;
	u8 scl;
	s8 mux;
	bool mux_high;
};

struct parhelia_ch7301 {
	struct parhelia_miscio_i2c *port;
	u16 addr;
	u8 vid;
};

/* 0x31..0x37 per pixel-clock band: ChrDVIParam, ChrVerBDVIParam and
 * ChrVerCDVIParam in mtx_drv.so, identical to rom0528 0x73d4..0x7427. */
static const u8 ch7301_dvi_default[7] = { 0x00, 0x2d, 0x07, 0x26, 0x30, 0xe0, 0x00 };
static const u8 ch7301_dvi_verb[3][7] = {
	{ 0x00, 0x23, 0x08, 0x16, 0x30, 0x10, 0x00 },
	{ 0x00, 0x23, 0x08, 0x16, 0x30, 0x60, 0x00 },
	{ 0x00, 0x2d, 0x06, 0x26, 0x30, 0xa0, 0x00 },
};
static const u8 ch7301_dvi_verc[2][7] = {
	{ 0x00, 0x23, 0x08, 0x16, 0x30, 0x60, 0x00 },
	{ 0x00, 0x2d, 0x06, 0x26, 0x70, 0xa0, 0x00 },
};

/* PInS[0x188] holds one pin id per nibble for pins 0..7, PInS[0x18c] the
 * id of pin 8 (rom0528 0x4cfa). */
static int parhelia_miscio_pin(const struct parhelia_bios *bios, u8 id)
{
	u32 ids = parhelia_pins_u32(bios, 0x188);
	unsigned int pin;

	for (pin = 0; pin < 8; pin++)
		if (((ids >> (4 * pin)) & 0xf) == id)
			return pin;
	if ((parhelia_pins_u32(bios, 0x18c) & 0xf) == id)
		return 8;
	return -ENOENT;
}

static void parhelia_miscio_set(struct parhelia_miscio_i2c *port, u8 pin,
				int high)
{
	struct parhelia_device *pdev = port->pdev;
	u32 v = parhelia_read(pdev, PARHELIA_REG_MISCIO);

	v &= ~(BIT(pin) | PARHELIA_MISCIO_OE(pin));
	if (!high)
		v |= PARHELIA_MISCIO_OE(pin);
	/* the data bits read back the pins, so re-assert the mux every write */
	if (port->mux >= 0) {
		v |= PARHELIA_MISCIO_OE(port->mux);
		if (port->mux_high)
			v |= BIT(port->mux);
		else
			v &= ~BIT(port->mux);
	}
	parhelia_write(pdev, PARHELIA_REG_MISCIO, v);
}

static void parhelia_miscio_setsda(void *data, int state)
{
	struct parhelia_miscio_i2c *port = data;

	parhelia_miscio_set(port, port->sda, state);
}

static void parhelia_miscio_setscl(void *data, int state)
{
	struct parhelia_miscio_i2c *port = data;

	parhelia_miscio_set(port, port->scl, state);
}

static int parhelia_miscio_getsda(void *data)
{
	struct parhelia_miscio_i2c *port = data;

	return !!(parhelia_read(port->pdev, PARHELIA_REG_MISCIO) &
		  BIT(port->sda));
}

static int parhelia_miscio_getscl(void *data)
{
	struct parhelia_miscio_i2c *port = data;

	return !!(parhelia_read(port->pdev, PARHELIA_REG_MISCIO) &
		  BIT(port->scl));
}

static int parhelia_miscio_pre_xfer(struct i2c_adapter *adap)
{
	struct parhelia_miscio_i2c *port = i2c_get_adapdata(adap);
	struct parhelia_device *pdev = port->pdev;

	unsigned int i;

	mutex_lock(&pdev->ddc_lock);
	parhelia_miscio_set(port, port->sda, 1);
	parhelia_miscio_set(port, port->scl, 1);
	udelay(10);
	/* a slave left mid-byte holds SDA low: clock it out, then STOP */
	for (i = 0; i < 9 && !parhelia_miscio_getsda(port); i++) {
		parhelia_miscio_set(port, port->scl, 0);
		udelay(5);
		parhelia_miscio_set(port, port->scl, 1);
		udelay(5);
	}
	if (i) {
		parhelia_miscio_set(port, port->scl, 0);
		parhelia_miscio_set(port, port->sda, 0);
		udelay(5);
		parhelia_miscio_set(port, port->scl, 1);
		udelay(5);
		parhelia_miscio_set(port, port->sda, 1);
		udelay(5);
		dev_warn_ratelimited(&pdev->pdev->dev,
				     "%s: released SDA after %u clocks\n",
				     port->adap.name, i);
	}
	return 0;
}

static void parhelia_miscio_post_xfer(struct i2c_adapter *adap)
{
	struct parhelia_miscio_i2c *port = i2c_get_adapdata(adap);
	struct parhelia_device *pdev = port->pdev;

	/* the select stays where the VBIOS and our DDC init leave it */
	parhelia_miscio_set(port, port->sda, 1);
	parhelia_miscio_set(port, port->scl, 1);
	mutex_unlock(&pdev->ddc_lock);
}

static void parhelia_miscio_release(struct drm_device *drm, void *res)
{
	struct parhelia_miscio_i2c *port = res;

	i2c_del_adapter(&port->adap);
}

static struct parhelia_miscio_i2c *
parhelia_miscio_port_init(struct parhelia_device *pdev, unsigned int index)
{
	const struct parhelia_miscio_port *desc = &parhelia_miscio_ports[index];
	struct parhelia_miscio_i2c *port;
	int sda, scl, mux;
	bool mux_high;

	if (!(parhelia_pins_u32(&pdev->bios, 0x88) & desc->gate))
		return NULL;
	sda = parhelia_miscio_pin(&pdev->bios, desc->sda_id);
	scl = parhelia_miscio_pin(&pdev->bios, desc->scl_id);
	if (sda < 0 || scl < 0)
		return NULL;
	/* channel select: id 5 driven low, else id 8 driven high (0x4cca) */
	mux = parhelia_miscio_pin(&pdev->bios, 5);
	mux_high = false;
	if (mux < 0) {
		mux = parhelia_miscio_pin(&pdev->bios, 8);
		mux_high = true;
	}

	port = drmm_kzalloc(&pdev->drm, sizeof(*port), GFP_KERNEL);
	if (!port)
		return ERR_PTR(-ENOMEM);
	port->pdev = pdev;
	port->sda = sda;
	port->scl = scl;
	port->mux = mux < 0 ? -1 : mux;
	port->mux_high = mux_high;
	port->bit.data = port;
	port->bit.setsda = parhelia_miscio_setsda;
	port->bit.setscl = parhelia_miscio_setscl;
	port->bit.getsda = parhelia_miscio_getsda;
	port->bit.getscl = parhelia_miscio_getscl;
	port->bit.pre_xfer = parhelia_miscio_pre_xfer;
	port->bit.post_xfer = parhelia_miscio_post_xfer;
	port->bit.udelay = 10;
	port->bit.timeout = usecs_to_jiffies(100000);
	port->adap.owner = THIS_MODULE;
	port->adap.algo_data = &port->bit;
	port->adap.dev.parent = pdev->drm.dev;
	snprintf(port->adap.name, sizeof(port->adap.name),
		 "Parhelia MISC-IO I2C (GPIO %u/%u)", scl, sda);
	i2c_set_adapdata(&port->adap, port);
	if (i2c_bit_add_bus(&port->adap))
		return NULL;
	if (drmm_add_action_or_reset(&pdev->drm, parhelia_miscio_release, port))
		return NULL;
	return port;
}

static int ch7301_read(struct parhelia_ch7301 *ch, u8 reg)
{
	u8 val;
	struct i2c_msg msgs[] = {
		{ .addr = ch->addr, .len = 1, .buf = &reg },
		{ .addr = ch->addr, .flags = I2C_M_RD, .len = 1, .buf = &val },
	};

	if (i2c_transfer(&ch->port->adap, msgs, ARRAY_SIZE(msgs)) != 2)
		return -EIO;
	return val;
}

static void ch7301_write(struct parhelia_ch7301 *ch, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = ch->addr, .len = 2, .buf = buf };

	i2c_transfer(&ch->port->adap, &msg, 1);
}

static void ch7301_toggle(struct parhelia_ch7301 *ch, u8 reg, u8 bit)
{
	int v = ch7301_read(ch, reg);

	if (v < 0)
		return;
	ch7301_write(ch, reg, v & ~bit);
	ch7301_write(ch, reg, v | bit);
}

/* Three consecutive lock reads within 150 polls, then 20 more in a row
 * (rom0528 0x7784..0x77b9). */
static bool ch7301_pll_stable(struct parhelia_ch7301 *ch)
{
	unsigned int polls, run = 0;
	int v;

	for (polls = 0; polls < 150 && run < 3; polls++) {
		v = ch7301_read(ch, CH7301_TSTAT);
		run = v >= 0 && (v & CH7301_TSTAT_LOCKED) ? run + 1 : 0;
	}
	if (run < 3)
		return false;
	for (run = 0; run < 20; run++) {
		v = ch7301_read(ch, CH7301_TSTAT);
		if (v < 0 || !(v & CH7301_TSTAT_LOCKED))
			return false;
	}
	return true;
}

/* rom0528 0x773a: arm the lock detector, then restart the data path up to
 * four times, and power-cycle the PLL once if that is not enough.  The
 * vendor HAL ORs bit 2 into 0x23; this board's VBIOS ORs bit 3, and that
 * is the state (0x23 = 0x0e) measured after both of its mode sets. */
static bool ch7301_pll_check(struct parhelia_ch7301 *ch)
{
	unsigned int restarts, cycles = 0;
	int v;

	v = ch7301_read(ch, CH7301_TCTL);
	if (v >= 0)
		ch7301_write(ch, CH7301_TCTL, v | CH7301_TCTL_PLL_LOCK);
	v = ch7301_read(ch, CH7301_TPVT);
	if (v >= 0)
		ch7301_write(ch, CH7301_TPVT, v | CH7301_TPVT_LOCK);

	for (;;) {
		for (restarts = 0; restarts < 4; restarts++) {
			if (ch7301_pll_stable(ch))
				return true;
			if (restarts < 3)
				ch7301_toggle(ch, CH7301_TSTP, CH7301_TSTP_RESETDB);
		}
		if (cycles++)
			return false;
		ch7301_toggle(ch, CH7301_PM, CH7301_PM_PLL);
		ch7301_toggle(ch, CH7301_TSTP, CH7301_TSTP_RESETDB);
	}
}

void parhelia_ch7301_enable(struct parhelia_device *pdev, unsigned int dvo,
			    u32 clock_khz)
{
	struct parhelia_ch7301 *ch = dvo < ARRAY_SIZE(pdev->ch7301) ?
				     pdev->ch7301[dvo] : NULL;
	const u8 *tpll = ch7301_dvi_default;
	int gpio, v;
	bool lock_check = false, locked = true;
	unsigned int i;

	if (!ch)
		return;

	/* HSLCHRReset(3): restart the data path, keep the GPIO high nibble */
	gpio = ch7301_read(ch, CH7301_GPIO);
	ch7301_write(ch, CH7301_TSTP, 0x08);
	ch7301_write(ch, CH7301_TSTP, 0x18);
	ch7301_write(ch, CH7301_TSTP, 0x10);
	ch7301_write(ch, CH7301_PM, 0x1e);
	v = ch7301_read(ch, CH7301_GPIO);
	if (gpio >= 0 && v >= 0)
		ch7301_write(ch, CH7301_GPIO, (v & 0x0f) | (gpio & 0xf0));

	ch7301_write(ch, CH7301_CM, 0x04);
	ch7301_write(ch, CH7301_IDF, 0x80);

	/* HSLCHREnableDigitalMode: DVI on, TMDS PLL tuned to the pixel clock */
	ch7301_write(ch, CH7301_PM, 0xde);
	switch (ch->vid) {
	case 0x85:
		i = clock_khz <= 31999 ? 0 : clock_khz < 65000 ? 1 : 2;
		tpll = ch7301_dvi_verb[i];
		lock_check = true;
		break;
	case 0x80:
	case 0x95:
		ch7301_write(ch, CH7301_TUNE, 0xa0);
		tpll = ch7301_dvi_verc[clock_khz > 64999];
		lock_check = true;
		break;
	}
	if (lock_check)
		ch7301_write(ch, CH7301_TCTL,
			     parhelia_pins_u32(&pdev->bios, 0x88) & 0x8000 ? 6 : 4);
	for (i = 0; i < ARRAY_SIZE(ch7301_dvi_default); i++)
		ch7301_write(ch, 0x31 + i, tpll[i]);
	ch7301_write(ch, CH7301_TSTP, 0x18);

	if (lock_check)
		locked = ch7301_pll_check(ch);
	/* Neither the VBIOS (0x77d7) nor HSLCHRPllCheck treats an exhausted
	 * check as an error, and 0x4d bit 2 was measured clear on a correct
	 * picture: it is a lock hint, not a link status. */
	if (!locked)
		dev_dbg(&pdev->pdev->dev,
			"CH7301 %#x: 0x4d lock hint stayed clear at %u kHz\n",
			ch->addr, clock_khz);
	dev_dbg(&pdev->pdev->dev,
		"CH7301 %#x for DVO%u at %u kHz: 0x32..0x36 %02x %02x %02x %02x %02x\n",
		ch->addr, dvo + 1, clock_khz, tpll[1], tpll[2], tpll[3],
		tpll[4], tpll[5]);
}

static struct parhelia_ch7301 *
parhelia_ch7301_probe(struct parhelia_device *pdev,
		      struct parhelia_miscio_i2c *port, u16 addr)
{
	struct parhelia_ch7301 tmp = { .port = port, .addr = addr };
	struct parhelia_ch7301 *ch;
	int vid;

	if (IS_ERR_OR_NULL(port))
		return NULL;
	vid = ch7301_read(&tmp, CH7301_VID);
	if (vid < 0)
		return NULL;
	ch = drmm_kzalloc(&pdev->drm, sizeof(*ch), GFP_KERNEL);
	if (!ch)
		return NULL;
	*ch = tmp;
	ch->vid = vid;
	dev_info(&pdev->pdev->dev, "Chrontel DVI transmitter %#x (VID %#04x) on %s\n",
		 addr, vid, port->adap.name);
	return ch;
}

/* rom0528 0x8514..0x8548: DVO1's transmitter answers at 0x76 on port 4, DVO2's
 * at 0x75 on port 4, else at 0x76 or 0x75 on port 5. */
int parhelia_ch7301_init(struct parhelia_device *pdev)
{
	struct parhelia_miscio_i2c *ports[ARRAY_SIZE(parhelia_miscio_ports)];
	unsigned int i;

	if (pdev->chip->output_family != PARHELIA_OUTPUT_ORIGINAL ||
	    !pdev->bios.pins_valid)
		return 0;
	for (i = 0; i < ARRAY_SIZE(ports); i++) {
		ports[i] = parhelia_miscio_port_init(pdev, i);
		if (IS_ERR(ports[i]))
			return PTR_ERR(ports[i]);
	}
	pdev->ch7301[0] = parhelia_ch7301_probe(pdev, ports[0], 0x76);
	pdev->ch7301[1] = parhelia_ch7301_probe(pdev, ports[0], 0x75);
	if (!pdev->ch7301[1])
		pdev->ch7301[1] = parhelia_ch7301_probe(pdev, ports[1], 0x76);
	if (!pdev->ch7301[1])
		pdev->ch7301[1] = parhelia_ch7301_probe(pdev, ports[1], 0x75);
	return 0;
}

/* Read-only register dump through the kernel's bus and lock, so no user tool
 * has to bit-bang 0x1c4c behind the driver's back. */
static int parhelia_ch7301_show(struct seq_file *m, void *data)
{
	static const u8 regs[] = { 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x23, 0x31,
				   0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x48, 0x49,
				   0x4a, 0x4b, 0x4d, 0x56, 0x70 };
	struct drm_info_node *node = m->private;
	struct parhelia_device *pdev = to_parhelia(node->minor->dev);
	unsigned int i, r;
	int v;

	for (i = 0; i < ARRAY_SIZE(pdev->ch7301); i++) {
		struct parhelia_ch7301 *ch = pdev->ch7301[i];

		if (!ch)
			continue;
		seq_printf(m, "DVO%u 0x%02x:", i + 1, ch->addr);
		for (r = 0; r < ARRAY_SIZE(regs); r++) {
			v = ch7301_read(ch, regs[r]);
			if (v < 0)
				seq_printf(m, " %02x:NAK", regs[r]);
			else
				seq_printf(m, " %02x=%02x", regs[r], v);
		}
		seq_putc(m, '\n');
	}
	seq_printf(m, "0x1c4c=%08x\n", parhelia_read(pdev, PARHELIA_REG_MISCIO));
	return 0;
}

static const struct drm_info_list parhelia_ch7301_debugfs_list[] = {
	{ "parhelia_ch7301", parhelia_ch7301_show, 0 },
};

void parhelia_ch7301_debugfs_init(struct drm_minor *minor)
{
	struct parhelia_device *pdev = to_parhelia(minor->dev);

	if (pdev->ch7301[0] || pdev->ch7301[1])
		drm_debugfs_create_files(parhelia_ch7301_debugfs_list,
					 ARRAY_SIZE(parhelia_ch7301_debugfs_list),
					 minor->debugfs_root, minor);
}
