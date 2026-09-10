// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/interrupt.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_rect.h>
#include <drm/drm_vblank.h>

#include "parhelia_drv.h"

static bool parhelia_clock_solve(struct parhelia_device *pdev, u32 clock,
				 u32 *word, u32 *actual);

static inline struct parhelia_output *
to_parhelia_output(struct drm_connector *connector)
{
	return container_of(connector, struct parhelia_output, connector);
}

static bool parhelia_output_load_sense(struct parhelia_output *output)
{
	struct parhelia_device *pdev = output->pdev;
	bool second = output->desc->analog_route == PARHELIA_ROUTE_DAC2;
	u32 offset = second ? PARHELIA_DC_STRIDE : 0;
	u32 dac_reg = second ? PARHELIA_REG_DAC2 : PARHELIA_REG_DAC1;
	u32 channels = second ? GENMASK(5, 3) : GENMASK(2, 0);
	u32 save_dac, save_test, save_mode, sense;

	save_dac = parhelia_read(pdev, dac_reg);
	save_test = parhelia_read(pdev, PARHELIA_REG_DAC_SENSE_TEST + offset);
	save_mode = parhelia_read(pdev, PARHELIA_REG_DAC_SENSE_MODE + offset);

	parhelia_write(pdev, dac_reg, save_dac | PARHELIA_DAC_SENSE_DISABLE);
	parhelia_write(pdev, PARHELIA_REG_DAC_SENSE_TEST + offset,
		       PARHELIA_DAC_SENSE_VALUE);
	parhelia_write(pdev, PARHELIA_REG_DAC_SENSE_MODE + offset,
		       save_mode & PARHELIA_DAC_SENSE_MODE_MASK);
	msleep(10);
	sense = parhelia_read(pdev, PARHELIA_REG_DAC_SENSE);

	parhelia_write(pdev, PARHELIA_REG_DAC_SENSE_MODE + offset, save_mode);
	parhelia_write(pdev, PARHELIA_REG_DAC_SENSE_TEST + offset, save_test);
	parhelia_write(pdev, dac_reg, save_dac);

	/* Each clear component bit means the corresponding DAC line is loaded. */
	return (sense & channels) != channels;
}

static int
parhelia_connector_detect(struct drm_connector *connector,
			  struct drm_modeset_acquire_ctx *ctx, bool force)
{
	struct parhelia_output *output = to_parhelia_output(connector);
	int status;

	status = drm_connector_helper_detect_from_ddc(connector, ctx, force);
	if (status == connector_status_connected) {
		output->analog_sensed = false;
		return status;
	}

	output->analog_sensed = parhelia_output_load_sense(output);
	return output->analog_sensed ? connector_status_connected : status;
}

static unsigned int parhelia_kms_head(struct parhelia_device *pdev)
{
	/* Keep the current single pipe on the connector descriptor's preferred
	 * controller.  On the tested P690, physical DVI-1 is DVO2 fed by
	 * controller B/head 1; DVI-2 is DVO1 fed by controller A/head 0. */
	if (pdev->num_outputs && pdev->outputs[0].desc)
		return pdev->outputs[0].desc->preferred_head;
	return PARHELIA_DC_HEAD0;
}

static u32 parhelia_vblank_mask(struct parhelia_device *pdev)
{
	/* P690's routed single display pipe reports retrace on the legacy display
	 * source even when DVI-1 is fed by controller B.  Bit 28 is the vendor
	 * multi-head object's second independent CRTC source; it produces no IRQ
	 * for this routing and makes atomic commits time out. */
	if (pdev->chip->output_family == PARHELIA_OUTPUT_P690)
		return PARHELIA_IRQ_VBLANK_HEAD0;
	/* OsSetVSyncInterruptState: bit 1=head 0, bit 28=head 1. */
	return parhelia_kms_head(pdev) ? BIT(28) : PARHELIA_IRQ_VBLANK_HEAD0;
}

/* The display controller owns a 256-entry RGB LUT.  The VBIOS sequence is
 * gate=0x08, index/R/G/B (blue carries the latch bit), gate=0x00.  The LUT is
 * physically ten bits wide.  HSLPARSetPrimaryLUT in the vendor driver writes
 * the high ten bits of each DRM 16-bit component directly into bits 15:6. */
static void parhelia_pipe_load_gamma(struct drm_simple_display_pipe *pipe,
				     const struct drm_property_blob *blob)
{
	struct parhelia_device *pdev = to_parhelia(pipe->crtc.dev);
	const struct drm_color_lut *lut = blob ? blob->data : NULL;
	u32 dc = PARHELIA_DC(parhelia_kms_head(pdev));
	unsigned int i;

	parhelia_write8(pdev, dc + PARHELIA_DC_LUT_GATE, 0x08);
	for (i = 0; i < 256; i++) {
		u16 red = lut ? lut[i].red : i * 257;
		u16 green = lut ? lut[i].green : i * 257;
		u16 blue = lut ? lut[i].blue : i * 257;
		u32 r = red & PARHELIA_DC_LUT_COMPONENT_MASK;
		u32 g = green & PARHELIA_DC_LUT_COMPONENT_MASK;
		u32 b = blue & PARHELIA_DC_LUT_COMPONENT_MASK;

		parhelia_write(pdev, dc + PARHELIA_DC_LUT_INDEX, i);
		parhelia_write(pdev, dc + PARHELIA_DC_LUT_R, r);
		parhelia_write(pdev, dc + PARHELIA_DC_LUT_G, g);
		parhelia_write(pdev, dc + PARHELIA_DC_LUT_B,
			       b | PARHELIA_DC_LUT_LATCH);
	}
	parhelia_write8(pdev, dc + PARHELIA_DC_LUT_GATE, 0x00);
}

static int parhelia_connector_get_modes(struct drm_connector *connector)
{
	struct parhelia_output *output = to_parhelia_output(connector);
	const struct drm_edid *drm_edid;
	int ret;

	drm_edid = drm_edid_read(connector);
	if (!drm_edid) {
		output->edid_valid = false;
		output->edid_digital = !output->analog_sensed &&
			connector->connector_type != DRM_MODE_CONNECTOR_VGA;
		drm_edid_connector_update(connector, NULL);
		if (connector->connector_type == DRM_MODE_CONNECTOR_DVII)
			drm_object_property_set_value(&connector->base,
				connector->dev->mode_config.dvi_i_subconnector_property,
				DRM_MODE_SUBCONNECTOR_Unknown);
		if (!output->analog_sensed)
			return 0;
		ret = drm_add_modes_noedid(connector, 2048, 2048);
		drm_set_preferred_mode(connector, 1024, 768);
		return ret;
	}

	output->edid_valid = true;
	output->edid_digital = drm_edid_is_digital(drm_edid);
	if (connector->connector_type == DRM_MODE_CONNECTOR_DVII)
		drm_object_property_set_value(&connector->base,
			connector->dev->mode_config.dvi_i_subconnector_property,
			output->edid_digital ? DRM_MODE_SUBCONNECTOR_DVID :
			DRM_MODE_SUBCONNECTOR_DVIA);
	ret = drm_edid_connector_update(connector, drm_edid);
	drm_edid_free(drm_edid);
	if (ret)
		return ret;

	return drm_edid_connector_add_modes(connector);
}

static const struct drm_connector_helper_funcs parhelia_connector_helper_funcs = {
	.get_modes = parhelia_connector_get_modes,
	.detect_ctx = parhelia_connector_detect,
};

static const struct drm_connector_funcs parhelia_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static enum drm_mode_status
parhelia_mode_valid(struct drm_simple_display_pipe *pipe,
			    const struct drm_display_mode *mode)
{
	struct parhelia_device *pdev = to_parhelia(pipe->crtc.dev);
	u32 word, actual;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		return MODE_NO_INTERLACE;
	if (mode->hdisplay < 320 || mode->vdisplay < 200 ||
	    mode->hdisplay > 2048 || mode->vdisplay > 2048)
		return MODE_BAD;
	if (mode->clock < 25000 || mode->clock > pdev->chip->max_pixel_clock_khz)
		return MODE_CLOCK_RANGE;
	if (!parhelia_clock_solve(pdev, mode->clock, &word, &actual))
		return MODE_CLOCK_RANGE;
	return MODE_OK;
}

/* HSLPAR PLL2 feeds native display-controller A on 0527/0528 and LX/Sunex.
 * The four negated divider fields and limits are recovered from the VBIOS.
 */
static bool parhelia_hslpar_clock_solve(u32 clock, u32 *word_out, u32 *actual_out)
{
	u32 best_error = ~0u, best_n = 0, best_p = 0, best_b3 = 0;
	u32 best_actual = 0, n, p, b3, word;

	for (n = 22; n <= 45; n++) {
		u32 vco1 = 27000 * n;

		if (vco1 < 600000 || vco1 > 1200000)
			continue;
		for (p = 1; p <= 31; p++) {
			u32 vco2 = vco1 * p / 3;

			if (vco2 < 600000 || vco2 > 1200000)
				continue;
			for (b3 = 2; b3 <= 62; b3 += 2) {
				u32 out = vco2 / b3;
				u32 error = out > clock ? out - clock : clock - out;

				if (error < best_error) {
					best_error = error;
					best_actual = out;
					best_n = n; best_p = p; best_b3 = b3;
				}
			}
		}
	}
	if (best_error == ~0u || (u64)best_error * 100 > clock)
		return false;

	word = ((best_n - 1) & 0x20) | ((-best_n) & 0x1f);
	word |= ((-best_p) & 0x1f) << 6;
	word |= ((-3) & 0x1f) << 11;
	word |= ((-best_b3) & 0x3f) << 16;
	*word_out = word;
	*actual_out = best_actual;
	return true;
}

/* SUN90 uses the FS bank at 0xe30, a 27 MHz reference and four positive
 * divider fields.  FS0 is the system clock and must never be touched; KMS head
 * 0 is deliberately pinned to FS2 (0xe38).
 */
static bool parhelia_sun90_clock_solve(u32 clock, u32 *word_out, u32 *actual_out)
{
	const u32 ref = 27000;
	u32 b0_lo = 800000 / ref, b0_hi = 1600000 / ref;
	u32 b2_lo = max_t(u32, (b0_lo * ref) / 108000, 3);
	u32 b2_hi = (b0_hi * ref) / 54000;
	u32 b1_lo = max_t(u32, (1600000 * b2_lo) / (b0_hi * ref), 3);
	u32 b1_hi = min_t(u32, (800000 * b2_hi) / (b0_lo * ref), 16);
	u32 pass;

	if (!clock)
		return false;

	for (pass = 0; pass < 2; pass++) {
		u32 threshold = pass ? 500 : 51;
		u32 b0, b1, b2, b3;

		for (b0 = b0_lo; b0 <= b0_hi; b0++)
		for (b1 = b1_lo; b1 <= b1_hi; b1++)
		for (b2 = b2_lo; b2 <= b2_hi; b2++)
		for (b3 = 1; b3 <= 16; b3++) {
			u32 vco1 = ref * b0;
			u32 tap = vco1 / b2;
			u32 vco2 = (ref * b0 * b1) / b2;
			u32 actual = vco2 / (2 * b3);
			u32 delta = actual > clock ? actual - clock : clock - actual;
			u32 error = div_u64((u64)delta * 10000, clock);

			if (error >= threshold || vco1 < 800000 || vco1 > 1600000 ||
			    tap < 54000 || tap > 108000 ||
			    vco2 < 800000 || vco2 > 1600000)
				continue;
			*word_out = ((b0 - 1) & 0x3f) |
				    (((b1 - 1) & 0x1f) << 6) |
				    (((b2 - 1) & 0x1f) << 11) |
				    (((b3 - 1) & 0x0f) << 16);
			*actual_out = actual;
			return true;
		}
	}
	return false;
}

static bool parhelia_clock_solve(struct parhelia_device *pdev, u32 clock,
				 u32 *word, u32 *actual)
{
	switch (pdev->chip->clock_family) {
	case PARHELIA_CLOCK_HSLPAR:
		return parhelia_hslpar_clock_solve(clock, word, actual);
	case PARHELIA_CLOCK_SUN90:
		return parhelia_sun90_clock_solve(clock, word, actual);
	default:
		return false;
	}
}

static void parhelia_route_dcm_a(struct parhelia_device *pdev, u32 clksel)
{
	u32 ctl = parhelia_read(pdev, PARHELIA_DCM_CLKCTL_A);

	ctl = (ctl & ~PARHELIA_DCM_CLKCTL_MUX_MASK) |
	      PARHELIA_DCM_CLKCTL_MUX_PLL | PARHELIA_DCM_CLKCTL_HOLD;
	parhelia_write(pdev, PARHELIA_DCM_CLKCTL_A, ctl);
	udelay(10);
	ctl &= ~(PARHELIA_DCM_CLKCTL_BIT10 | PARHELIA_DCM_CLKCTL_SEL_MASK);
	ctl |= clksel << PARHELIA_DCM_CLKCTL_SEL_SHIFT;
	parhelia_write(pdev, PARHELIA_DCM_CLKCTL_A, ctl);
	parhelia_write(pdev, PARHELIA_DCM_CLKCTL_A,
		       ctl & ~PARHELIA_DCM_CLKCTL_HOLD);
}

static int parhelia_program_clock(struct parhelia_device *pdev, u32 clock,
				  unsigned int head)
{
	u32 word, actual, ctl;
	unsigned int i;

	if (!parhelia_clock_solve(pdev, clock, &word, &actual))
		return -EINVAL;
	pdev->last_clock_khz = 0;

	switch (pdev->chip->clock_family) {
	case PARHELIA_CLOCK_HSLPAR:
		parhelia_write(pdev, 0x0e18, word | BIT(31));
		udelay(100);
		parhelia_write(pdev, 0x0e18, word);
		udelay(200);
		parhelia_route_dcm_a(pdev, 3);
		break;
	case PARHELIA_CLOCK_SUN90:
		/* Head 0 uses FS2; P690 DVI-1/head 1 uses FS3.  FS0 is the system
		 * clock and must never be touched. */
		ctl = head ? 0x0e3c : 0x0e38;
		parhelia_write(pdev, ctl, word | BIT(31) | BIT(21));
		parhelia_write(pdev, ctl, word);
		udelay(1000);
		for (i = 0; i < 100; i++) {
			if (parhelia_read(pdev, ctl) & BIT(24))
				break;
			udelay(10);
		}
		if (i == 100)
			return -ETIMEDOUT;
		if (head) {
			parhelia_route_dcm_a(pdev, 4);
			/* Controller B feeds head 1; route it to the same FS3. */
			ctl = parhelia_read(pdev, PARHELIA_DCM_CLKCTL_B);
			ctl = (ctl & ~PARHELIA_DCM_CLKCTL_MUX_MASK) |
			      PARHELIA_DCM_CLKCTL_MUX_PLL |
			      PARHELIA_DCM_CLKCTL_HOLD;
			parhelia_write(pdev, PARHELIA_DCM_CLKCTL_B, ctl);
			udelay(10);
			ctl &= ~(PARHELIA_DCM_CLKCTL_BIT10 |
				 PARHELIA_DCM_CLKCTL_SEL_MASK);
			ctl |= 4 << PARHELIA_DCM_CLKCTL_SEL_SHIFT;
			parhelia_write(pdev, PARHELIA_DCM_CLKCTL_B, ctl);
			parhelia_write(pdev, PARHELIA_DCM_CLKCTL_B,
				       ctl & ~PARHELIA_DCM_CLKCTL_HOLD);
		} else {
			parhelia_route_dcm_a(pdev, 3);
		}
		parhelia_write(pdev, PARHELIA_DCM_PRIORITY_A,
			       PARHELIA_DCM_PRIORITY_ON);
		parhelia_write(pdev, PARHELIA_DCM_PRIORITY_B,
			       PARHELIA_DCM_PRIORITY_ON);
		/* Match HSLPARSelectDisplayControllerClock and the hardware-tested
		 * DDX handoff.  The native DCM selector above chooses the particular
		 * FS for each head; the VGA MISC mux must independently select the
		 * programmable-FS clock family rather than retain a POST VGA clock. */
		ctl = parhelia_read8(pdev, PARHELIA_VGA_MISC_R);
		ctl = (ctl & ~PARHELIA_VGA_MISC_CLKSEL_MASK) |
		      PARHELIA_VGA_MISC_CLKSEL_FS;
		parhelia_write8(pdev, PARHELIA_VGA_MISC_W, ctl);
		break;
	default:
		return -ENODEV;
	}

	ctl = parhelia_read(pdev, PARHELIA_DCM_CLKCTL_HEAD);
	if (!(ctl & PARHELIA_DCM_MODE_HIRES)) {
		parhelia_write(pdev, PARHELIA_DCM_CLKCTL_HEAD,
			       ctl & ~PARHELIA_DCM_MODE_VGA);
		udelay(10);
		parhelia_write(pdev, PARHELIA_DCM_CLKCTL_HEAD,
			       (ctl & ~PARHELIA_DCM_MODE_VGA) |
			       PARHELIA_DCM_MODE_HIRES);
	}
	dev_dbg(&pdev->pdev->dev,
		"%s pixel clock %u kHz -> %u kHz, word %#08x\n",
		pdev->chip->name, clock, actual, word);
	pdev->last_clock_khz = actual;
	return 0;
}

static void parhelia_program_output(struct parhelia_device *pdev,
				    const struct drm_display_mode *mode,
				    unsigned int head)
{
	struct parhelia_output *output = &pdev->outputs[0];
	u32 dac_reg, power_mask, v, fmt;

	if (pdev->chip->output_family == PARHELIA_OUTPUT_P690 &&
	    output->edid_digital &&
	    output->desc->digital_route == PARHELIA_ROUTE_DVO2) {
		/* DVI-I monitors can choose analog while its DAC is powered.  Shut
		 * both DACs down before starting the integrated TMDS serializer. */
		v = parhelia_read(pdev, PARHELIA_REG_DAC1);
		parhelia_write(pdev, PARHELIA_REG_DAC1, v | 0x0000bc00);
		v = parhelia_read(pdev, PARHELIA_REG_DAC2);
		parhelia_write(pdev, PARHELIA_REG_DAC2, v | 0x00002800);

		/* Release the board-level output gate observed in the working P690
		 * VBE state, then reconfigure DVO2 while powered down and clock-gated. */
		v = parhelia_read(pdev, PARHELIA_REG_OUTPUT_GATE);
		parhelia_write(pdev, PARHELIA_REG_OUTPUT_GATE,
			       v & ~PARHELIA_OUTPUT_GATE_TMDS);
		v = parhelia_read(pdev, PARHELIA_REG_DVO_PWR);
		parhelia_write(pdev, PARHELIA_REG_DVO_PWR, v | BIT(1));
		v = parhelia_read(pdev, PARHELIA_DCM_CLKCTL_B);
		v |= BIT(14);
		v = (v & ~(0xfu << 6)) | ((head ? 4u : 3u) << 6);
		v = (v & ~(7u << 15)) | ((head ? 4u : 3u) << 15);
		v |= 0x00030000u;
		parhelia_write(pdev, PARHELIA_DCM_CLKCTL_B, v);

		/* Subsystem 102b:0040 uses the standard P690 DVO wiring.  Preserve
		 * the three live power bits when installing its board configuration. */
		v = parhelia_read(pdev, PARHELIA_REG_DVO_PWR);
		parhelia_write(pdev, PARHELIA_REG_DVO_PWR,
			       (0xaac00000u & ~7u) | (v & 7u));
		parhelia_write(pdev, PARHELIA_REG_DVO_DATA, 0x10);

		fmt = head ? BIT(30) : 0;
		if (mode->clock > 165000)
			fmt |= BIT(25);
		parhelia_write(pdev, PARHELIA_REG_DVO_FORMAT2, fmt);
		/* DVO2 sync polarity has its own formatter register.  On P690 the
		 * active-low VGA convention is represented by clear bits; do not copy
		 * those bits into FORMAT2, whose high byte selects the source/head. */
		v = fmt & ~0x28000000u;
		if (!(mode->flags & DRM_MODE_FLAG_NHSYNC))
			v |= BIT(27);
		if (!(mode->flags & DRM_MODE_FLAG_NVSYNC))
			v |= BIT(29);
		parhelia_write(pdev, PARHELIA_REG_DVO_SYNC2, v);
		v = parhelia_read(pdev, PARHELIA_REG_DVO_XBAR);
		v &= ~(0x28000000u | 0x00002800u);
		if (!(mode->flags & DRM_MODE_FLAG_NHSYNC))
			v |= BIT(11);
		if (!(mode->flags & DRM_MODE_FLAG_NVSYNC))
			v |= BIT(13);
		parhelia_write(pdev, PARHELIA_REG_DVO_XBAR, v);

		v = parhelia_read(pdev, PARHELIA_DCM_CLKCTL_B);
		parhelia_write(pdev, PARHELIA_DCM_CLKCTL_B, v & ~BIT(14));
		v = parhelia_read(pdev, PARHELIA_REG_DVO_PWR);
		parhelia_write(pdev, PARHELIA_REG_DVO_PWR, v & ~6u);
		udelay(200);
		return;
	}

	/* DAC controls are shared across A-E.  The connector descriptor identifies
	 * which converter is physically wired to its analog pins. */
	switch (pdev->chip->output_family) {
	case PARHELIA_OUTPUT_ORIGINAL:
	case PARHELIA_OUTPUT_LX:
	case PARHELIA_OUTPUT_P690:
		break;
	default:
		return;
	}
	dac_reg = output->desc->analog_route == PARHELIA_ROUTE_DAC1 ?
		PARHELIA_REG_DAC1 : PARHELIA_REG_DAC2;
	power_mask = output->desc->analog_route == PARHELIA_ROUTE_DAC1 ?
		PARHELIA_DAC1_PWR : PARHELIA_DAC2_PWR;
	v = parhelia_read(pdev, dac_reg);
	v &= ~(PARHELIA_DAC_ROUTE_DCMB | PARHELIA_DAC_HSYNC_NEG |
	       PARHELIA_DAC_VSYNC_NEG | power_mask);
	if (head)
		v |= PARHELIA_DAC_ROUTE_DCMB;
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		v |= PARHELIA_DAC_HSYNC_NEG;
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		v |= PARHELIA_DAC_VSYNC_NEG;
	parhelia_write(pdev, dac_reg, v);
	udelay(200);
}

static void parhelia_apply_vid_reset(struct parhelia_device *pdev,
				      unsigned int head)
{
	u32 clk, route, dct = PARHELIA_DCT(head);
	u32 route_reg = head ? PARHELIA_DCM_CLKCTL_B : PARHELIA_DCM_CLKCTL_A;
	u32 ctl_a, ctl_b;

	ctl_a = parhelia_read(pdev, PARHELIA_DCM_CLKCTL_A) &
		~PARHELIA_DCM_CLKCTL_HOLD;
	ctl_b = parhelia_read(pdev, PARHELIA_DCM_CLKCTL_B) &
		~PARHELIA_DCM_CLKCTL_HOLD;
	parhelia_write(pdev, PARHELIA_DCM_CLKCTL_A, ctl_a);
	parhelia_write(pdev, PARHELIA_DCM_CLKCTL_B, ctl_b);
	parhelia_write(pdev, dct + PARHELIA_DCT_MISC,
		       parhelia_read(pdev, dct + PARHELIA_DCT_MISC) |
		       PARHELIA_DCT_RESET);
	route = parhelia_read(pdev, route_reg) | PARHELIA_DCM_CLKCTL_RSTROUTE;
	parhelia_write(pdev, route_reg, route);
	clk = parhelia_read(pdev, PARHELIA_DCM_CLKCTL_A);
	parhelia_write(pdev, PARHELIA_DCM_CLKCTL_A,
		       clk & ~PARHELIA_DCM_CLKCTL_RSTSTROBE);
	udelay(10);
	parhelia_write(pdev, PARHELIA_DCM_CLKCTL_A,
		       clk | PARHELIA_DCM_CLKCTL_RSTSTROBE);
	parhelia_write(pdev, PARHELIA_DCM_CLKCTL_A,
		       clk & ~PARHELIA_DCM_CLKCTL_RSTSTROBE);
	parhelia_write(pdev, route_reg,
		       parhelia_read(pdev, route_reg) &
		       ~PARHELIA_DCM_CLKCTL_RSTROUTE);
	parhelia_write(pdev, dct + PARHELIA_DCT_MISC,
		       parhelia_read(pdev, dct + PARHELIA_DCT_MISC) &
		       ~PARHELIA_DCT_RESET);
}

static unsigned int parhelia_scanout_tile_height(struct parhelia_device *pdev)
{
	return pdev->chip->tile_layout == PARHELIA_TILE_64X32_8K ? 32 : 16;
}

static size_t parhelia_scanout_pixel_offset(struct parhelia_device *pdev,
					    u32 x, u32 y, u32 pitch)
{
	u32 tile_h = parhelia_scanout_tile_height(pdev);
	u32 tile_bytes = 64 * tile_h * 4;
	u32 tiles_per_row = pitch / 64;
	u32 tile_x = x / 64;
	u32 tile_y = y / tile_h;
	u32 ix = x & 63;
	u32 iy = y & (tile_h - 1);
	u32 x5 = ((ix >> 5) & 1) ^ (tile_y & 1);
	u32 i;

	i = (ix & 1) |
	    (((ix >> 1) & 1) << 1) |
	    (((ix >> 2) & 1) << 2) |
	    (((iy >> 1) & 1) << 3) |
	    (((ix >> 3) & 1) << 4) |
	    (((ix >> 4) & 1) << 5) |
	    ((iy & 1) << 6) |
	    (x5 << 7) |
	    (((iy >> 2) & 1) << 8) |
	    (((iy >> 3) & 1) << 9);
	if (tile_h == 32)
		i |= ((iy >> 4) & 1) << 10;

	return ((size_t)tile_y * tiles_per_row + tile_x) * tile_bytes + i * 4;
}

static void parhelia_scanout_release(struct parhelia_device *pdev)
{
	struct drm_gem_vram_object *bo = pdev->scanout_bo;

	if (!bo)
		return;
	if (!ttm_bo_reserve(&bo->bo, false, false, NULL)) {
		drm_gem_vram_vunmap(bo, &pdev->scanout_map);
		ttm_bo_unreserve(&bo->bo);
	}
	parhelia_vram_unpin(bo);
	drm_gem_vram_put(bo);
	pdev->scanout_bo = NULL;
	iosys_map_clear(&pdev->scanout_map);
	kfree(pdev->scanout_tile);
	pdev->scanout_tile = NULL;
	pdev->scanout_size = 0;
	pdev->scanout_pitch = 0;
	pdev->scanout_height = 0;
}

void parhelia_scanout_fini(struct parhelia_device *pdev)
{
	parhelia_scanout_release(pdev);
}

static int parhelia_scanout_ensure(struct parhelia_device *pdev,
				   u32 pitch, u32 height)
{
	struct drm_gem_vram_object *bo;
	struct iosys_map map = { };
	u32 tile_h = parhelia_scanout_tile_height(pdev);
	u32 alloc_height = ALIGN(height, tile_h);
	size_t size = PAGE_ALIGN((size_t)pitch * alloc_height * 4);
	u8 *tile;
	int ret;

	if (pdev->scanout_bo && pdev->scanout_pitch == pitch &&
	    pdev->scanout_height >= alloc_height)
		return 0;

	bo = drm_gem_vram_create(&pdev->drm, size,
				 pdev->chip->tile_layout == PARHELIA_TILE_64X32_8K ? 2 : 1);
	if (IS_ERR(bo))
		return PTR_ERR(bo);
	ret = parhelia_vram_pin(bo);
	if (ret)
		goto err_put;
	ret = ttm_bo_reserve(&bo->bo, true, false, NULL);
	if (ret)
		goto err_unpin;
	ret = drm_gem_vram_vmap(bo, &map);
	ttm_bo_unreserve(&bo->bo);
	if (ret)
		goto err_unpin;
	tile = kmalloc(64 * tile_h * 4, GFP_KERNEL);
	if (!tile) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	parhelia_scanout_release(pdev);
	pdev->scanout_bo = bo;
	pdev->scanout_map = map;
	pdev->scanout_tile = tile;
	pdev->scanout_size = size;
	pdev->scanout_pitch = pitch;
	pdev->scanout_height = alloc_height;
	return 0;

err_unmap:
	if (!ttm_bo_reserve(&bo->bo, false, false, NULL)) {
		drm_gem_vram_vunmap(bo, &map);
		ttm_bo_unreserve(&bo->bo);
	}
err_unpin:
	parhelia_vram_unpin(bo);
err_put:
	drm_gem_vram_put(bo);
	return ret;
}

static void parhelia_scanout_copy(struct parhelia_device *pdev,
				  struct drm_framebuffer *fb,
				  const struct iosys_map *src,
				  const struct drm_rect *damage, bool fresh)
{
	struct drm_gem_object *src_gem = drm_gem_fb_get_obj(fb, 0);
	struct drm_gem_vram_object *src_bo = src_gem ?
		drm_gem_vram_of_gem(src_gem) : NULL;
	s64 src_offset = src_bo ? drm_gem_vram_offset(src_bo) : -ENODEV;
	s64 dst_offset = pdev->scanout_bo ?
		drm_gem_vram_offset(pdev->scanout_bo) : -ENODEV;
	u32 tile_h = parhelia_scanout_tile_height(pdev);
	u32 tile_bytes = 64 * tile_h * 4;
	u32 first_tx = damage->x1 / 64;
	u32 last_tx = DIV_ROUND_UP(damage->x2, 64);
	u32 first_ty = damage->y1 / tile_h;
	u32 last_ty = DIV_ROUND_UP(damage->y2, tile_h);
	u32 tx, ty;
	int blit_ret = -ENODEV;

	if (!pdev->scanout_bo || !pdev->scanout_tile || !src)
		return;

	/* The normal path is an engine resolve ordered with all render submits.
	 * Keep the mapped tile conversion below as a correctness fallback for the
	 * classic 64x32 chips until their blit layout is verified. */
	if (src_offset >= 0 && dst_offset >= 0)
		blit_ret = parhelia_submit_linear_to_tiled(pdev, src_offset,
					       fb->pitches[0] / 4, dst_offset,
					       pdev->scanout_pitch,
					       damage->x1, damage->y1,
					       drm_rect_width(damage),
					       drm_rect_height(damage));
	if (!blit_ret) {
		dev_info_once(&pdev->pdev->dev,
			      "using GPU linear-to-native KMS damage resolves\n");
		wmb();
		return;
	}
	dev_warn_once(&pdev->pdev->dev,
		      "GPU KMS resolve unavailable (%d, src=%lld dst=%lld); using CPU tile conversion\n",
		      blit_ret, src_offset, dst_offset);

	for (ty = first_ty; ty < last_ty; ty++) {
		for (tx = first_tx; tx < last_tx; tx++) {
			size_t tile_base = ((size_t)ty * (pdev->scanout_pitch / 64) + tx) *
					   tile_bytes;
			u32 x1 = max_t(u32, damage->x1, tx * 64);
			u32 x2 = min_t(u32, damage->x2, min_t(u32, fb->width, (tx + 1) * 64));
			u32 y1 = max_t(u32, damage->y1, ty * tile_h);
			u32 y2 = min_t(u32, damage->y2, min_t(u32, fb->height,
								   (ty + 1) * tile_h));
			u32 x, y;

			if (fresh)
				memset(pdev->scanout_tile, 0, tile_bytes);
			else
				iosys_map_memcpy_from(pdev->scanout_tile,
						      &pdev->scanout_map, tile_base,
						      tile_bytes);
			for (y = y1; y < y2; y++) {
				u8 row[64 * 4];

				iosys_map_memcpy_from(row, src,
						      (size_t)y * fb->pitches[0] + x1 * 4,
						      (x2 - x1) * 4);
				for (x = x1; x < x2; x++) {
					size_t off = parhelia_scanout_pixel_offset(pdev, x, y,
									      pdev->scanout_pitch);

					memcpy(pdev->scanout_tile + off - tile_base,
					       row + (x - x1) * 4, 4);
				}
			}
			iosys_map_memcpy_to(&pdev->scanout_map, tile_base,
					    pdev->scanout_tile, tile_bytes);
		}
	}
	wmb();
}

static void parhelia_pipe_enable(struct drm_simple_display_pipe *pipe,
				 struct drm_crtc_state *crtc_state,
				 struct drm_plane_state *plane_state)
{
	struct parhelia_device *pdev = to_parhelia(pipe->crtc.dev);
	struct drm_display_mode *m = &crtc_state->adjusted_mode;
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_shadow_plane_state *shadow;
	struct drm_rect full;
	s64 offset;
	u32 dc;
	u32 dct;
	u32 format;
	unsigned int head = parhelia_kms_head(pdev);
	u32 pitch;

	dc = PARHELIA_DC(head);
	dct = PARHELIA_DCT(head);

	if (!fb)
		return;
	/* Stop fetch before replacing or releasing the old private scanout BO. */
	parhelia_write(pdev, dc + PARHELIA_DC_CTRL,
		       parhelia_read(pdev, dc + PARHELIA_DC_CTRL) & ~1);
	pitch = roundup_pow_of_two(max_t(u32, fb->width, 64));
	if (parhelia_scanout_ensure(pdev, pitch, fb->height)) {
		dev_err(&pdev->pdev->dev, "cannot allocate tiled scanout for %ux%u\n",
			fb->width, fb->height);
		return;
	}
	shadow = to_drm_shadow_plane_state(plane_state);
	full = DRM_RECT_INIT(0, 0, fb->width, fb->height);
	parhelia_scanout_copy(pdev, fb, &shadow->data[0], &full, true);
	offset = drm_gem_vram_offset(pdev->scanout_bo);
	if (offset < 0)
		return;

	format = fb->format->format == DRM_FORMAT_RGB565 ? 0x28000000 : 0x32000000;
	/* Type 3 is the native display/render-target layout. */
	format |= 0x00030000 | pdev->scanout_pitch;

	/* BAR mappings and PRIME buffers retain ordinary linear semantics. */
	parhelia_write(pdev,
		       PARHELIA_HOST_APERTURE_SLOT(PARHELIA_HOST_APERTURE_KMS_SLOT), 0);

	if (parhelia_program_clock(pdev, m->clock, head)) {
		dev_err(&pdev->pdev->dev,
			"%s pixel PLL failed to lock at %u kHz\n",
			pdev->chip->name, m->clock);
		return;
	}
	parhelia_write(pdev, dct + PARHELIA_DCT_MISC, 0);
	parhelia_write(pdev, dct + PARHELIA_DCT_HTIMING,
		       ((m->htotal - 2) << 16) | (m->hdisplay - 2));
	parhelia_write(pdev, dct + PARHELIA_DCT_HTIMING2,
		       ((m->hdisplay - 2) << 16) | (m->htotal - 2));
	parhelia_write(pdev, dct + PARHELIA_DCT_HSYNC,
		       ((m->hsync_end - 2) << 16) | (m->hsync_start - 2));
	parhelia_write(pdev, dct + PARHELIA_DCT_VTIMING,
		       ((m->vtotal - 1) << 16) | (m->vdisplay - 1));
	parhelia_write(pdev, dct + PARHELIA_DCT_VTIMING2,
		       ((m->vdisplay - 1) << 16) | (m->vtotal - 1));
	parhelia_write(pdev, dct + PARHELIA_DCT_VSYNC,
		       ((m->vsync_end - 1) << 16) | (m->vsync_start - 1));
	parhelia_write(pdev, dct + PARHELIA_DCT_VTOTAL, m->vtotal - 1);
	parhelia_write(pdev, dct + PARHELIA_DCT_VBLANK2, 0);
	parhelia_write(pdev, dct + PARHELIA_DCT_FIELD, 0);
	parhelia_write(pdev, dc + PARHELIA_DC_XPAN, 0);
	parhelia_write(pdev, dc + PARHELIA_DC_HDISP, (m->hdisplay - 1) << 16);
	parhelia_write(pdev, dc + PARHELIA_DC_YPAN, 0);
	parhelia_write(pdev, dc + PARHELIA_DC_VDISP, m->vdisplay - 1);
	parhelia_write(pdev, dc + PARHELIA_DC_START, offset & 0x0ffffc00);
	parhelia_write(pdev, dc + PARHELIA_DC_FORMAT, format);
	parhelia_pipe_load_gamma(pipe, crtc_state->gamma_lut);
	/* Do not mirror this into MMIO 0x0004/0x0008.  The complete engine
	 * initializer proves those words are host-aperture descriptors; changing
	 * them after engine ownership redirects 3D writes outside the surface. */
	parhelia_program_output(pdev, m, head);
	if (pdev->chip->clock_family == PARHELIA_CLOCK_SUN90) {
		u32 params = parhelia_read(pdev, dc + PARHELIA_DC_PARAMS);

		params = (params & ~PARHELIA_DC_DIM_EN) | PARHELIA_DC_VIDEO_EN;
		parhelia_write(pdev, dc + PARHELIA_DC_PARAMS, params);
	}
	parhelia_write(pdev, dc + PARHELIA_DC_CTRL, PARHELIA_DC_CTRL_ON);
	parhelia_apply_vid_reset(pdev, head);
	/* The P690 reset transient can clear DC_CTRL's enable bit. */
	parhelia_write(pdev, dc + PARHELIA_DC_CTRL, PARHELIA_DC_CTRL_ON);
	/* DRM's simple KMS helper delegates this lifecycle transition to drivers. */
	drm_crtc_vblank_on(&pipe->crtc);
}

static void parhelia_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct parhelia_device *pdev = to_parhelia(pipe->crtc.dev);
	u32 dc = PARHELIA_DC(parhelia_kms_head(pdev));

	drm_crtc_vblank_off(&pipe->crtc);
	parhelia_write(pdev, dc + PARHELIA_DC_CTRL,
		       parhelia_read(pdev, dc + PARHELIA_DC_CTRL) & ~1);
}

static void parhelia_pipe_update(struct drm_simple_display_pipe *pipe,
				  struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct parhelia_device *pdev = to_parhelia(pipe->crtc.dev);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(state);
	struct drm_pending_vblank_event *event;
	struct drm_rect damage;

	if (state->fb && pipe->crtc.state->active) {
		if (drm_atomic_helper_damage_merged(old_state, state, &damage))
			parhelia_scanout_copy(pdev, state->fb, &shadow->data[0],
					      &damage, false);
	}
	if (pipe->crtc.state->color_mgmt_changed)
		parhelia_pipe_load_gamma(pipe, pipe->crtc.state->gamma_lut);

	/* DC_START latches at retrace.  Transfer the atomic event to DRM's
	 * vblank queue before commit_hw_done(), then the IRQ path timestamps and
	 * completes it on the matching retrace. */
	spin_lock_irq(&pipe->crtc.dev->event_lock);
	event = pipe->crtc.state->event;
	pipe->crtc.state->event = NULL;
	if (event) {
		if (drm_crtc_vblank_get(&pipe->crtc) == 0)
			drm_crtc_arm_vblank_event(&pipe->crtc, event);
		else
			drm_crtc_send_vblank_event(&pipe->crtc, event);
	}
	spin_unlock_irq(&pipe->crtc.dev->event_lock);
}

static int parhelia_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct parhelia_device *pdev = to_parhelia(pipe->crtc.dev);
	u32 mask;

	u32 vblank = parhelia_vblank_mask(pdev);

	parhelia_write(pdev, PARHELIA_MMIO_IRQ_ACK, vblank);
	mask = parhelia_read(pdev, PARHELIA_MMIO_IRQ_MASK) |
	       vblank;
	parhelia_write(pdev, PARHELIA_MMIO_IRQ_MASK, mask);
	return 0;
}

static void parhelia_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct parhelia_device *pdev = to_parhelia(pipe->crtc.dev);

	u32 vblank = parhelia_vblank_mask(pdev);

	parhelia_write(pdev, PARHELIA_MMIO_IRQ_MASK,
		       parhelia_read(pdev, PARHELIA_MMIO_IRQ_MASK) &
		       ~vblank);
	parhelia_write(pdev, PARHELIA_MMIO_IRQ_ACK, vblank);
}

irqreturn_t parhelia_irq_handler(int irq, void *arg)
{
	struct parhelia_device *pdev = arg;
	u32 status = parhelia_read(pdev, PARHELIA_MMIO_IRQ_STATUS);

	if (!status || status == ~0u)
		return IRQ_NONE;
	parhelia_write(pdev, PARHELIA_MMIO_IRQ_ACK, status);
	if (status & parhelia_vblank_mask(pdev))
		drm_crtc_handle_vblank(&pdev->pipe.crtc);
	return IRQ_HANDLED;
}

static int parhelia_pipe_check(struct drm_simple_display_pipe *pipe,
			       struct drm_plane_state *plane_state,
			       struct drm_crtc_state *crtc_state)
{
	struct drm_framebuffer *fb = plane_state->fb;
	u32 cpp, pitch_pixels;

	if (crtc_state->color_mgmt_changed) {
		if (crtc_state->gamma_lut &&
		    drm_color_lut_size(crtc_state->gamma_lut) != 256)
			return -EINVAL;
		/* A gamma-only atomic commit has no framebuffer change.  Make the
		 * simple-pipe helper invoke update(), where the LUT is latched. */
		crtc_state->planes_changed = true;
	}

	if (!fb)
		return 0;
	cpp = fb->format->cpp[0];
	if (cpp != 4 || fb->pitches[0] % cpp)
		return -EINVAL;
	pitch_pixels = fb->pitches[0] / cpp;
	/* Userspace storage is linear and may have any valid stride.  The private
	 * scanout image gets the fetcher's required power-of-two pitch. */
	if (pitch_pixels < fb->width ||
	    roundup_pow_of_two(max_t(u32, fb->width, 64)) > 0x3fff)
		return -EINVAL;
	return 0;
}

static int parhelia_pipe_begin_fb_access(struct drm_simple_display_pipe *pipe,
					 struct drm_plane_state *plane_state)
{
	int ret;

	/* Pin the linear source in VRAM for the engine fast path, then establish
	 * the CPU mapping retained by the layout-conversion fallback. */
	ret = drm_gem_vram_plane_helper_prepare_fb(&pipe->plane, plane_state);
	if (ret)
		return ret;
	ret = drm_gem_simple_kms_begin_shadow_fb_access(pipe, plane_state);
	if (ret)
		drm_gem_vram_plane_helper_cleanup_fb(&pipe->plane, plane_state);
	return ret;
}

static void parhelia_pipe_end_fb_access(struct drm_simple_display_pipe *pipe,
					struct drm_plane_state *plane_state)
{
	drm_gem_simple_kms_end_shadow_fb_access(pipe, plane_state);
	drm_gem_vram_plane_helper_cleanup_fb(&pipe->plane, plane_state);
}

static const struct drm_simple_display_pipe_funcs parhelia_pipe_funcs = {
	.mode_valid = parhelia_mode_valid,
	.check = parhelia_pipe_check,
	.enable = parhelia_pipe_enable,
	.disable = parhelia_pipe_disable,
	.update = parhelia_pipe_update,
	.enable_vblank = parhelia_pipe_enable_vblank,
	.disable_vblank = parhelia_pipe_disable_vblank,
	.begin_fb_access = parhelia_pipe_begin_fb_access,
	.end_fb_access = parhelia_pipe_end_fb_access,
	.reset_plane = drm_gem_simple_kms_reset_shadow_plane,
	.duplicate_plane_state = drm_gem_simple_kms_duplicate_shadow_plane_state,
	.destroy_plane_state = drm_gem_simple_kms_destroy_shadow_plane_state,
};

static const uint32_t parhelia_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const struct drm_mode_config_funcs parhelia_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* P690 and LX boards use the first low-byte GPIO pair for their primary
 * DVI-I connector and the second pair for DVI-2.  The original 0527/0528
 * boards use the alternate recovered pair assignment.  P690 DVI-2 is exposed
 * for DDC/EDID now that both buses are hardware-proven, but remains detached
 * from the single display pipe until its independent output route is enabled.
 * Other unverified board outputs stay unadvertised. */
static const struct parhelia_output_desc parhelia_p690_outputs[] = {
	{
		.connector_type = DRM_MODE_CONNECTOR_DVII,
		.ddc_line = 0,
		.ddc_scl = 1, .ddc_sda = 0,
		.preferred_head = 1,
		.digital_route = PARHELIA_ROUTE_DVO2,
		.analog_route = PARHELIA_ROUTE_DAC1,
		.expose = true,
		.ready = true,
	}, {
		.connector_type = DRM_MODE_CONNECTOR_DVII,
		.ddc_line = 1,
		.ddc_scl = 3, .ddc_sda = 2,
		.preferred_head = 0,
		.digital_route = PARHELIA_ROUTE_DVO1,
		.analog_route = PARHELIA_ROUTE_DAC2,
		.expose = true,
		.ready = false,
	},
};

static const struct parhelia_output_desc parhelia_lx_outputs[] = {
	{
		.connector_type = DRM_MODE_CONNECTOR_VGA,
		.ddc_line = 0,
		.ddc_scl = 1, .ddc_sda = 0,
		.preferred_head = 0,
		.digital_route = PARHELIA_ROUTE_DVO2,
		.analog_route = PARHELIA_ROUTE_DAC2,
		.expose = true,
		.ready = true,
	}, {
		.connector_type = DRM_MODE_CONNECTOR_DVII,
		.ddc_line = 1,
		.ddc_scl = 3, .ddc_sda = 2,
		.preferred_head = 1,
		.digital_route = PARHELIA_ROUTE_DVO1,
		.analog_route = PARHELIA_ROUTE_DAC1,
		.expose = false,
		.ready = false,
	},
};

static const struct parhelia_output_desc parhelia_original_outputs[] = {
	{
		.connector_type = DRM_MODE_CONNECTOR_VGA,
		.ddc_line = 0,
		.ddc_scl = 3, .ddc_sda = 1,
		.preferred_head = 0,
		.digital_route = PARHELIA_ROUTE_DVO2,
		.analog_route = PARHELIA_ROUTE_DAC2,
		.expose = true,
		.ready = true,
	}, {
		.connector_type = DRM_MODE_CONNECTOR_DVII,
		.ddc_line = 1,
		.ddc_scl = 2, .ddc_sda = 0,
		.preferred_head = 1,
		.digital_route = PARHELIA_ROUTE_DVO1,
		.analog_route = PARHELIA_ROUTE_DAC1,
		.expose = false,
		.ready = false,
	},
};

static const struct parhelia_output_desc *
parhelia_output_descriptors(struct parhelia_device *pdev, unsigned int *count)
{
	switch (pdev->chip->output_family) {
	case PARHELIA_OUTPUT_P690:
		*count = ARRAY_SIZE(parhelia_p690_outputs);
		return parhelia_p690_outputs;
	case PARHELIA_OUTPUT_LX:
		*count = ARRAY_SIZE(parhelia_lx_outputs);
		return parhelia_lx_outputs;
	case PARHELIA_OUTPUT_ORIGINAL:
		*count = ARRAY_SIZE(parhelia_original_outputs);
		return parhelia_original_outputs;
	default:
		*count = 0;
		return NULL;
	}
}

static int parhelia_output_init(struct parhelia_device *pdev,
				struct parhelia_output *output,
				const struct parhelia_output_desc *desc)
{
	struct drm_device *drm = &pdev->drm;
	int ret;

	output->pdev = pdev;
	output->desc = desc;
	output->ddc_scl = desc->ddc_scl;
	output->ddc_sda = desc->ddc_sda;
	if (pdev->bios.pins_valid &&
	    !parhelia_bios_ddc_pins(&pdev->bios, desc->ddc_line,
				     &output->ddc_scl, &output->ddc_sda))
		dev_warn(&pdev->pdev->dev,
			 "PInS DDC line %u cannot be resolved; using GPIO %u/%u\n",
			 desc->ddc_line, output->ddc_scl, output->ddc_sda);
	output->edid_digital = desc->connector_type != DRM_MODE_CONNECTOR_VGA;

	ret = parhelia_ddc_init(output);
	if (ret)
		return ret;

	ret = drm_connector_init_with_ddc(drm, &output->connector,
					  &parhelia_connector_funcs,
					  desc->connector_type, &output->ddc);
	if (ret)
		return ret;
	drm_connector_helper_add(&output->connector,
				 &parhelia_connector_helper_funcs);
	if (desc->connector_type == DRM_MODE_CONNECTOR_DVII)
		drm_object_attach_property(&output->connector.base,
			drm->mode_config.dvi_i_subconnector_property,
			DRM_MODE_SUBCONNECTOR_Unknown);
	output->connector.polled = DRM_CONNECTOR_POLL_CONNECT |
				   DRM_CONNECTOR_POLL_DISCONNECT;

	if (desc->ready)
		return drm_connector_attach_encoder(&output->connector,
						    &pdev->pipe.encoder);
	return 0;
}

int parhelia_modeset_init(struct parhelia_device *pdev)
{
	struct drm_device *drm = &pdev->drm;
	const struct parhelia_output_desc *descs;
	unsigned int count, i;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &parhelia_mode_config_funcs;
	/* Framebuffer limits apply to cursor framebuffers as well as scanout.
	 * Keep the lower bound small enough for the 64x64 ARGB cursor BO; mode
	 * limits are enforced separately by the connector/pipe checks. */
	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;
	drm->mode_config.max_width = 2048;
	drm->mode_config.max_height = 2048;
	drm->mode_config.prefer_shadow = 1;
	ret = drm_mode_create_dvi_i_properties(drm);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(drm, &pdev->pipe, &parhelia_pipe_funcs,
					   parhelia_formats, ARRAY_SIZE(parhelia_formats),
					   NULL, NULL);
	if (ret)
		return ret;
	drm_plane_enable_fb_damage_clips(&pdev->pipe.plane);
	ret = parhelia_cursor_init(pdev);
	if (ret)
		return ret;

	mutex_init(&pdev->ddc_lock);
	descs = parhelia_output_descriptors(pdev, &count);
	for (i = 0; i < count; i++) {
		if (!descs[i].expose)
			continue;
		if (pdev->num_outputs >= ARRAY_SIZE(pdev->outputs))
			return -E2BIG;
		ret = parhelia_output_init(pdev, &pdev->outputs[pdev->num_outputs],
					   &descs[i]);
		if (ret)
			return ret;
		pdev->num_outputs++;
	}
	if (!pdev->num_outputs)
		return -ENODEV;
	ret = drm_mode_crtc_set_gamma_size(&pdev->pipe.crtc, 256);
	if (ret)
		return ret;
	drm_crtc_enable_color_mgmt(&pdev->pipe.crtc, 0, false, 256);
	drm_mode_config_reset(drm);
	return 0;
}
