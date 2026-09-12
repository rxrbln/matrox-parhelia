// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/delay.h>
#include <linux/interrupt.h>

#include <drm/drm_atomic.h>
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

/* The output whose connector an atomic commit has attached to this CRTC.  The
 * single pipe has one encoder shared by every connector, so the commit's
 * connector is what says which physical output to program. */
static struct parhelia_output *
parhelia_commit_output(struct parhelia_device *pdev,
		       struct drm_crtc_state *crtc_state)
{
	struct drm_connector *connector = NULL;

	if (crtc_state->state)
		connector = drm_atomic_get_new_connector_for_encoder(
			crtc_state->state, &pdev->pipe.encoder);
	if (connector)
		return to_parhelia_output(connector);
	return pdev->num_outputs ? &pdev->outputs[0] : NULL;
}

static unsigned int parhelia_kms_head(struct parhelia_device *pdev)
{
	/* Keep the current single pipe on the connector descriptor's preferred
	 * controller.  On the tested P690, physical DVI-1 is DVO2 fed by
	 * controller B/head 1; DVI-2 is DVO1 fed by controller A/head 0. */
	if (pdev->active_output && pdev->active_output->desc)
		return pdev->active_output->desc->preferred_head;
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

/* The display controller owns a 256-entry RGB LUT, physically ten bits wide.
 * HSLPARSetPrimaryLUT writes index, then R, G and B per entry with blue
 * carrying the latch bit, each component masked 0xffc0 - the high ten bits of
 * a 16-bit DRM component land in bits 15:6. */
static void parhelia_pipe_load_gamma(struct drm_simple_display_pipe *pipe,
				     const struct drm_property_blob *blob)
{
	struct parhelia_device *pdev = to_parhelia(pipe->crtc.dev);
	const struct drm_color_lut *lut = blob ? blob->data : NULL;
	u32 dc = PARHELIA_DC(parhelia_kms_head(pdev));
	unsigned int i;

	/* No vendor path brackets the LUT with a gate byte, and dc + 0x1aa is not
	 * a gate: at dc 0x1800 it is byte 2 of 0x19a8, the *second* controller's
	 * DC_CTRL, so the pair of writes set and cleared that controller's bit 19
	 * (parhelia-p690-output-path-re.md section 5.9). */
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
		/* Without EDID, offer the standard list when the board senses
		 * an analog load or when userspace forced the connector on.
		 * This board answers DDC on neither line - its SCL pin reads
		 * stuck low - so forcing is the only way to light it up. */
		if (!output->analog_sensed &&
		    connector->force != DRM_FORCE_ON &&
		    connector->force != DRM_FORCE_ON_DIGITAL)
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

/* HSLPAR: the display controller's own PLL2 word.  This is the divider model
 * our userspace driver has working on this board (xf86-video-parhelia
 * src/parhelia_mode.c parhelia_program_dc_clock): a four-field negated word
 * N[5:0] P[10:6] X[15:11] B3[21:16] with the stage-1 divider X fixed at 3, as
 * in both VBIOS golden words - 108 MHz is N24/P3/X3/B3=6 and 65 MHz is
 * N23/P5/X3/B3=16 - and B3 the even output post-divider.  Above 135 MHz head 0
 * runs at the VCO1/X tap rather than through B3, so that case is solved
 * directly; a numerically correct VCO2/B3 word comes out at twice the wanted
 * frequency there.
 *
 * The system-clock synthesisers use the same silicon fields but are searched
 * differently (parhelia_fs_solve(), docs/parhelia-cold-post-bringup-re.md
 * §4.4); this keeps the pixel path on the values proven on this hardware.
 */
static bool parhelia_hslpar_clock_solve(u32 clock, u32 *word_out,
					u32 *actual_out)
{
	const u32 ref = 27000;
	u32 best = ~0u, out = 0, word;
	u32 n, p, b3, n_b = 0, p_b = 0, x_b = 0, b3_b = 0;

	if (!clock)
		return false;
	if (clock > 135000) {
		u32 x;

		for (x = 2; x <= 12 && best; x++)
			for (n = 22; n <= 45; n++) {
				u32 vco1 = ref * n;

				if (vco1 < 600000 || vco1 > 1200000)
					continue;
				if (vco1 / x == clock) {
					n_b = n; x_b = x; p_b = 3; b3_b = 6;
					out = clock;
					best = 0;
					break;
				}
			}
	}
	for (n = 22; n <= 45 && best; n++) {
		u32 vco1 = ref * n, tap;

		if (vco1 < 600000 || vco1 > 1200000)
			continue;
		tap = vco1 / 3;
		if (tap < 50000 || tap > 400000)
			continue;
		for (p = 1; p <= 31; p++) {
			u32 vco2 = (ref * n * p) / 3;

			if (vco2 < 600000 || vco2 > 1200000)
				continue;
			for (b3 = 2; b3 <= 62; b3 += 2) {
				u32 fout = vco2 / b3;
				u32 err = abs_diff(fout, clock);

				if (err >= best)
					continue;
				best = err;
				n_b = n; p_b = p; x_b = 3; b3_b = b3;
				out = fout;
			}
		}
	}
	if (best == ~0u)
		return false;

	word = ((n_b - 1) & 0x20) | ((-n_b) & 0x1f);
	word |= ((-p_b) & 0x1f) << 6;
	word |= ((-x_b) & 0x1f) << 11;
	word |= ((-b3_b) & 0x3f) << 16;
	*word_out = word;
	*actual_out = out;
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

/* The controller enable word and the scanout-base field are per family. */
static u32 parhelia_dc_ctrl_on(struct parhelia_device *pdev)
{
	return pdev->chip->output_family == PARHELIA_OUTPUT_ORIGINAL ?
		PARHELIA_DC_CTRL_ON_ORIGINAL : PARHELIA_DC_CTRL_ON_SUN90;
}

static u32 parhelia_dc_start_mask(struct parhelia_device *pdev)
{
	return pdev->chip->output_family == PARHELIA_OUTPUT_ORIGINAL ?
		PARHELIA_DC_START_MASK_ORIGINAL : PARHELIA_DC_START_MASK;
}

/* Point one display controller's clock mux at a synthesiser: controller A
 * (0x1c00) feeds head 0 and controller B (0x1c04) feeds head 1, so a driver
 * that only ever wrote A could only ever light up head 0. */
static void parhelia_route_dcm(struct parhelia_device *pdev, unsigned int head,
			       u32 clksel)
{
	u32 reg = head ? PARHELIA_DCM_CLKCTL_B : PARHELIA_DCM_CLKCTL_A;
	u32 ctl = parhelia_read(pdev, reg);

	ctl = (ctl & ~PARHELIA_DCM_CLKCTL_MUX_MASK) |
	      PARHELIA_DCM_CLKCTL_MUX_PLL | PARHELIA_DCM_CLKCTL_HOLD;
	parhelia_write(pdev, reg, ctl);
	udelay(10);
	ctl &= ~(PARHELIA_DCM_CLKCTL_BIT10 | PARHELIA_DCM_CLKCTL_SEL_MASK);
	ctl |= clksel << PARHELIA_DCM_CLKCTL_SEL_SHIFT;
	parhelia_write(pdev, reg, ctl);
	parhelia_write(pdev, reg, ctl & ~PARHELIA_DCM_CLKCTL_HOLD);
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
		/* Preserve the bits neither vendor writer ever sets. */
		ctl = parhelia_read(pdev, 0x0e18) &
		      (BIT(22) | BIT(23) | BIT(29));
		parhelia_write(pdev, 0x0e18, ctl | word | BIT(31));
		udelay(100);
		parhelia_write(pdev, 0x0e18, ctl | word);
		udelay(200);
		if (!parhelia_fs_locked(pdev, 0x0e18, 90))
			dev_warn(&pdev->pdev->dev,
				 "pixel synthesiser reports no lock at %u kHz (word %#010x)\n",
				 actual, word);
		/* Route this head's own controller, then select the DC head's
		 * clock source: without this write head 1's clock never
		 * engaged on hardware. */
		parhelia_route_dcm(pdev, head, 3);
		/* Controller A, and only controller A, additionally takes the
		 * VGA MISC clock-select byte: HSLPARSelectDisplayControllerClock
		 * writes 0x68 there on that branch, and a head left on the
		 * legacy clock explains a locked synthesiser with a dark
		 * monitor.  Controller B gets no MISC write at all. */
		if (!head)
			parhelia_write8(pdev, PARHELIA_VGA_MISC_W, 0x68);
		parhelia_write(pdev, PARHELIA_DCM_CLKCTL_HEAD,
			       PARHELIA_DCM_MODE_HIRES);
		udelay(200);
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
			parhelia_route_dcm(pdev, 0, 4);
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
			parhelia_route_dcm(pdev, head, 3);
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
				    struct parhelia_output *output,
				    const struct drm_display_mode *mode,
				    unsigned int head)
{
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
	/* DAC1 selects its display controller with bit 14 alone; DAC2 uses the
	 * two bits [15:14], where bit 15 set alone would select the TV clock
	 * source instead.  Heads 0 and 1 are modules 0 and 1, so the low bit
	 * carries the head and the high bit must be clear. */
	if (output->desc->analog_route == PARHELIA_ROUTE_DAC2)
		v &= ~(u32)PARHELIA_DAC2_MODULE_MASK;
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

/* DC_START takes the same bank-encoded address as the engine's SURF
 * operands: bank A is identity, bank B starts at 128 MiB.  A flat BAR offset
 * into bank B leaves the controller fetching stale memory (P690 test with the
 * native colour class at 64 MiB physical). */
static int parhelia_scanout_address(struct parhelia_device *pdev,
				    struct drm_gem_object *gem, u32 *address)
{
	int ret;

	ret = parhelia_vram_gpu_addr(pdev, drm_gem_vram_of_gem(gem), 0,
				     gem->size, address);
	if (!ret && (*address > parhelia_dc_start_mask(pdev) ||
		     !IS_ALIGNED(*address, 1024)))
		ret = -ERANGE;
	if (ret)
		dev_warn_ratelimited(&pdev->pdev->dev,
				     "scanout BO has no valid display address: %d\n",
				     ret);
	return ret;
}

/* DC_FORMAT's surface code, the same field the 3-D engine's surface
 * configuration carries: 0x32 for A8R8G8B8, 0x28 for R5G6B5 and 0x34 for the
 * 30-bit GigaColor word (docs/parhelia-p690-gigacolor-10bit.md).
 */
static u32 parhelia_dc_surface_code(u32 drm_format)
{
	switch (drm_format) {
	case DRM_FORMAT_RGB565:
		return 0x28000000u;
	case DRM_FORMAT_XRGB2101010:
		return 0x34000000u;
	default:
		return 0x32000000u;
	}
}

static void parhelia_pipe_enable(struct drm_simple_display_pipe *pipe,
				 struct drm_crtc_state *crtc_state,
				 struct drm_plane_state *plane_state)
{
	struct parhelia_device *pdev = to_parhelia(pipe->crtc.dev);
	struct drm_display_mode *m = &crtc_state->adjusted_mode;
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_gem_object *gem;
	struct parhelia_bo *bo;
	u32 address;
	u32 dc;
	u32 dct;
	u32 format;
	unsigned int head;
	u32 pitch;

	/* Drive the connector this commit enabled, not whichever one happens
	 * to be first in the descriptor table. */
	pdev->active_output = parhelia_commit_output(pdev, crtc_state);
	head = parhelia_kms_head(pdev);
	dc = PARHELIA_DC(head);
	dct = PARHELIA_DCT(head);

	if (!fb)
		return;
	/* Stop fetch before selecting the new native surface. */
	parhelia_write(pdev, dc + PARHELIA_DC_CTRL,
		       parhelia_read(pdev, dc + PARHELIA_DC_CTRL) & ~1);
	gem = drm_gem_fb_get_obj(fb, 0);
	if (!gem)
		return;
	bo = to_parhelia_bo(gem);
	if (parhelia_scanout_address(pdev, gem, &address))
		return;
	pitch = bo->gpu_pitch / fb->format->cpp[0];

	format = parhelia_dc_surface_code(fb->format->format);
	/* Type 3 is the native display/render-target layout. */
	format |= 0x00030000 | pitch;
	/* CPU stores through the host aperture must reach VRAM before scanout is
	 * enabled.  There is no second image and no layout resolve.
	 */
	wmb();

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
	parhelia_write(pdev, dc + PARHELIA_DC_START,
		       address & parhelia_dc_start_mask(pdev));
	parhelia_write(pdev, dc + PARHELIA_DC_FORMAT, format);
	parhelia_pipe_load_gamma(pipe, crtc_state->gamma_lut);
	/* Do not mirror this into MMIO 0x0004/0x0008.  The complete engine
	 * initializer proves those words are host-aperture descriptors; changing
	 * them after engine ownership redirects 3D writes outside the surface. */
	/* Both request-level fields of this head's DEMREQCTL carry 3 at every
	 * vendor mode set; SwitchToHIRES parks head A at zero, and a
	 * controller with no memory request level never fetches a scanline. */
	if (pdev->chip->output_family == PARHELIA_OUTPUT_ORIGINAL) {
		u32 reg = head ? PARHELIA_DCM_PRIORITY_B :
				 PARHELIA_DCM_PRIORITY_A;
		u32 v = parhelia_read(pdev, reg);

		v = (v & ~PARHELIA_DCM_PRIORITY_MASK) |
		    PARHELIA_DCM_PRIORITY_LEVELS;
		parhelia_write(pdev, reg, v);
	}
	parhelia_program_output(pdev, pdev->active_output, m, head);
	/* Release the dim/fade blank this family asserts around the timing
	 * writes; POST can leave it asserted, which is a black picture with a
	 * running raster. */
	if (pdev->chip->output_family == PARHELIA_OUTPUT_ORIGINAL) {
		u32 dim = PARHELIA_DC_DIM + head * PARHELIA_DC_STRIDE;

		parhelia_write(pdev, dim,
			       parhelia_read(pdev, dim) &
			       ~(u32)PARHELIA_DC_DIM_ASSERT);
	}
	if (pdev->chip->clock_family == PARHELIA_CLOCK_SUN90) {
		u32 params = parhelia_read(pdev, dc + PARHELIA_DC_PARAMS);

		params = (params & ~PARHELIA_DC_DIM_EN) | PARHELIA_DC_VIDEO_EN;
		parhelia_write(pdev, dc + PARHELIA_DC_PARAMS, params);
	}
	parhelia_write(pdev, dc + PARHELIA_DC_CTRL, parhelia_dc_ctrl_on(pdev));
	parhelia_apply_vid_reset(pdev, head);
	/* The P690 reset transient can clear DC_CTRL's enable bit. */
	parhelia_write(pdev, dc + PARHELIA_DC_CTRL, parhelia_dc_ctrl_on(pdev));
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
	struct drm_pending_vblank_event *event;

	if (state->fb && pipe->crtc.state->active) {
		/* A dirty update only publishes WC writes already made through the
		 * surface's linear aperture.  A real framebuffer change selects the
		 * new native surface and is latched by the normal atomic/vblank path.
		 */
		wmb();
		if (!old_state->fb || old_state->fb != state->fb) {
			struct drm_gem_object *gem = drm_gem_fb_get_obj(state->fb, 0);
			struct parhelia_bo *bo = gem ? to_parhelia_bo(gem) : NULL;
			u32 address;

			if (gem && !parhelia_scanout_address(pdev, gem, &address)) {
				u32 dc = PARHELIA_DC(parhelia_kms_head(pdev));
				u32 cpp = state->fb->format->cpp[0];
				u32 format =
					parhelia_dc_surface_code(state->fb->format->format) |
					0x00030000u | (bo->gpu_pitch / cpp);

				parhelia_write(pdev, dc + PARHELIA_DC_START,
					       address &
					       parhelia_dc_start_mask(pdev));
				parhelia_write(pdev, dc + PARHELIA_DC_FORMAT, format);
			}
		}
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
	/* Fence waiters re-read FENCE_DONE themselves; retirement needs the
	 * submit mutex and runs from the workqueue. */
	if (status & PARHELIA_IRQ_ENGINE) {
		atomic64_inc(&pdev->submit_stats.engine_irqs);
		wake_up_all(&pdev->submit_waitq);
		mod_delayed_work(system_wq, &pdev->retire_work, 0);
	}
	return IRQ_HANDLED;
}

static int parhelia_pipe_check(struct drm_simple_display_pipe *pipe,
			       struct drm_plane_state *plane_state,
			       struct drm_crtc_state *crtc_state)
{
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_gem_object *gem;
	struct parhelia_bo *bo;
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
	gem = drm_gem_fb_get_obj(fb, 0);
	if (!gem || !parhelia_gem_is_owned(gem))
		return -EINVAL;
	bo = to_parhelia_bo(gem);
	if (!parhelia_bo_is_native(bo))
		return -EINVAL;
	cpp = fb->format->cpp[0];
	/* A 16 bpp surface tiles 128 pixels wide; 32 bpp tiles 64. */
	if ((cpp != 4 && cpp != 2) || fb->pitches[0] % cpp)
		return -EINVAL;
	pitch_pixels = fb->pitches[0] / cpp;
	if (pitch_pixels != (bo->host_aperture ? bo->host_pitch : bo->gpu_pitch) / cpp ||
	    bo->gpu_pitch / cpp != ALIGN(fb->width, 64 * 4 / cpp) ||
	    bo->gpu_pitch / cpp > 0x3fff)
		return -EINVAL;
	/* Atomic check runs before begin_fb_access() pins the new framebuffer.
	 * drm_gem_vram_offset() is therefore invalid here and would both WARN and
	 * reject an otherwise valid first modeset.  The VRAM helper pins and
	 * places the BO before enable()/update(); those callbacks validate the
	 * resulting controller address before programming DC_START.
	 */
	return 0;
}

static int parhelia_pipe_begin_fb_access(struct drm_simple_display_pipe *pipe,
					 struct drm_plane_state *plane_state)
{
	return drm_gem_vram_plane_helper_prepare_fb(&pipe->plane, plane_state);
}

static void parhelia_pipe_end_fb_access(struct drm_simple_display_pipe *pipe,
					struct drm_plane_state *plane_state)
{
	drm_gem_vram_plane_helper_cleanup_fb(&pipe->plane, plane_state);
}

/* An asynchronous flip only reprograms the surface address.  The P690 latches
 * DC_START at the next retrace, so the image never changes mid-scanline; the
 * tearing an asynchronous flip permits comes from releasing the old buffer to
 * the client at once.  Everything else must already match the committed state. */
static int parhelia_plane_async_check(struct drm_plane *plane,
				      struct drm_atomic_commit *state,
				      bool flip)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct parhelia_device *pdev = to_parhelia(plane->dev);
	struct drm_gem_object *gem, *old_gem;
	u32 address;

	if (!new_state->fb || !plane->state->fb || !plane->state->crtc ||
	    new_state->crtc != plane->state->crtc ||
	    !plane->state->crtc->state->active ||
	    new_state->crtc_x != plane->state->crtc_x ||
	    new_state->crtc_y != plane->state->crtc_y ||
	    new_state->crtc_w != plane->state->crtc_w ||
	    new_state->crtc_h != plane->state->crtc_h ||
	    new_state->src_x != plane->state->src_x ||
	    new_state->src_y != plane->state->src_y ||
	    new_state->src_w != plane->state->src_w ||
	    new_state->src_h != plane->state->src_h ||
	    new_state->rotation != plane->state->rotation ||
	    new_state->fb->format != plane->state->fb->format ||
	    new_state->fb->pitches[0] != plane->state->fb->pitches[0])
		return -EINVAL;
	gem = drm_gem_fb_get_obj(new_state->fb, 0);
	old_gem = drm_gem_fb_get_obj(plane->state->fb, 0);
	if (!gem || !old_gem ||
	    to_parhelia_bo(gem)->gpu_pitch != to_parhelia_bo(old_gem)->gpu_pitch)
		return -EINVAL;
	return parhelia_scanout_address(pdev, gem, &address);
}

static void parhelia_plane_async_update(struct drm_plane *plane,
					struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct parhelia_device *pdev = to_parhelia(plane->dev);
	struct drm_crtc *crtc = plane->state->crtc;
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_gem_object *gem = drm_gem_fb_get_obj(new_state->fb, 0);
	u32 dc = PARHELIA_DC(parhelia_kms_head(pdev));
	u32 address;

	swap(plane->state->fb, new_state->fb);
	if (gem && !parhelia_scanout_address(pdev, gem, &address)) {
		wmb();
		parhelia_write(pdev, dc + PARHELIA_DC_START,
			       address & 0x0ffffc00);
		atomic64_inc(&pdev->submit_stats.async_flips);
	}
	/* The asynchronous helper does not process events.  Report completion as
	 * soon as the address is programmed: that is what the caller asked for by
	 * setting DRM_MODE_PAGE_FLIP_ASYNC, and it is what releases the old buffer
	 * to the client.  The visible change still happens at the next retrace. */
	if (crtc_state && crtc_state->event) {
		spin_lock_irq(&plane->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc_state->event);
		crtc_state->event = NULL;
		spin_unlock_irq(&plane->dev->event_lock);
	}
}

/* drm_atomic_helper_page_flip() and the atomic ioctl only record
 * drm_crtc_state.async_flip; without a driver commit tail that acts on it an
 * asynchronous flip would be committed like any other one and completed at the
 * next vblank.  Promote such a commit to the helper's asynchronous path when
 * the plane accepts it, so the flip is programmed and reported immediately. */
static int parhelia_atomic_commit(struct drm_device *drm,
				  struct drm_atomic_commit *state, bool nonblock)
{
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	int i;

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		if (!crtc_state->async_flip)
			continue;
		if (!drm_atomic_helper_async_check(drm, state))
			state->async_update = true;
		break;
	}
	return drm_atomic_helper_commit(drm, state, nonblock);
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
};

static const uint32_t parhelia_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB2101010,
};

static int parhelia_fb_dirty(struct drm_framebuffer *fb,
			     struct drm_file *file, unsigned int flags,
			     unsigned int color, struct drm_clip_rect *clips,
			     unsigned int num_clips)
{
	/* The framebuffer is already the scanout surface.  DIRTYFB is only the
	 * CPU-write publication point and must never enter an atomic commit tail.
	 */
	wmb();
	return 0;
}

static const struct drm_framebuffer_funcs parhelia_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
	.dirty = parhelia_fb_dirty,
};

static struct drm_framebuffer *
parhelia_fb_create(struct drm_device *drm, struct drm_file *file,
		   const struct drm_format_info *info,
		   const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_gem_object *gem;
	struct parhelia_bo *bo;
	struct drm_framebuffer *fb;

	if ((info->format != DRM_FORMAT_XRGB8888 &&
	     info->format != DRM_FORMAT_ARGB8888 &&
	     info->format != DRM_FORMAT_RGB565 &&
	     info->format != DRM_FORMAT_XRGB2101010) || mode_cmd->offsets[0])
		return ERR_PTR(-EINVAL);
	gem = drm_gem_object_lookup(file, mode_cmd->handles[0]);
	if (!gem)
		return ERR_PTR(-ENOENT);
	if (!parhelia_gem_is_owned(gem)) {
		fb = ERR_PTR(-EINVAL);
		goto out_put;
	}
	bo = to_parhelia_bo(gem);
	if (!parhelia_bo_is_native(bo) ||
	    (bo->host_aperture ? bo->host_pitch : bo->gpu_pitch) !=
		mode_cmd->pitches[0] ||
	    bo->width < mode_cmd->width || bo->height < mode_cmd->height) {
		fb = ERR_PTR(-EINVAL);
		goto out_put;
	}
	fb = drm_gem_fb_create_with_funcs(drm, file, info, mode_cmd,
					  &parhelia_fb_funcs);
out_put:
	drm_gem_object_put(gem);
	return fb;
}

static const struct drm_mode_config_funcs parhelia_mode_config_funcs = {
	.fb_create = parhelia_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = parhelia_atomic_commit,
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

/* The retail 0527/0528 boards carry two DVI-I connectors and no VGA socket:
 * an analog monitor reaches them through a DVI-I adapter on the DAC pins, so
 * both connectors are DVI-I and each keeps its own DAC route.  Declaring the
 * first one VGA also suppressed its DVI-I subconnector property and forced
 * output->edid_digital false before any EDID was read. */
static const struct parhelia_output_desc parhelia_original_outputs[] = {
	{
		.connector_type = DRM_MODE_CONNECTOR_DVII,
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
		.expose = true,
		.ready = true,
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
	/* Dumb buffers are native surfaces scanned out directly.  Their mmap is a
	 * CPU-linear hardware aperture alias of those same bytes, so userspace can
	 * draw without ShadowFB and DIRTYFB needs only publish WC stores.
	 */
	drm->mode_config.prefer_shadow = 0;
	ret = drm_mode_create_dvi_i_properties(drm);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(drm, &pdev->pipe, &parhelia_pipe_funcs,
					   parhelia_formats, ARRAY_SIZE(parhelia_formats),
					   NULL, NULL);
	if (ret)
		return ret;
	drm_plane_enable_fb_damage_clips(&pdev->pipe.plane);
	/* Asynchronous flips: the simple pipe has no such hooks of its own. */
	pdev->plane_helper_funcs = *pdev->pipe.plane.helper_private;
	pdev->plane_helper_funcs.atomic_async_check = parhelia_plane_async_check;
	pdev->plane_helper_funcs.atomic_async_update = parhelia_plane_async_update;
	drm_plane_helper_add(&pdev->pipe.plane, &pdev->plane_helper_funcs);
	drm->mode_config.async_page_flip = true;
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
