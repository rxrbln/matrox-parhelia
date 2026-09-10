// SPDX-License-Identifier: GPL-2.0-only
/* Matrox Parhelia 64x64 palette/alpha hardware cursor. */
#include <linux/iopoll.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_plane_helper.h>

#include "parhelia_drv.h"

#define PARHELIA_CURSOR_WIDTH 64
#define PARHELIA_CURSOR_HEIGHT 64
#define PARHELIA_CURSOR_STRIDE 128
#define PARHELIA_CURSOR_SIZE (PARHELIA_CURSOR_STRIDE * PARHELIA_CURSOR_HEIGHT)

#define PARHELIA_CURSOR_BASE(head) (PARHELIA_DC(head) + 0x00c0)
#define PARHELIA_CURSOR_COLOR 0x0000
#define PARHELIA_CURSOR_CTL 0x0004
#define PARHELIA_CURSOR_POS 0x0008
#define PARHELIA_CURSOR_ADDR 0x000c
#define PARHELIA_CURSOR_CFG 0x0010
#define PARHELIA_CURSOR_CFG_16COLOR 0x04000000
#define PARHELIA_CURSOR_CTL_16COLOR 0x00000004
#define PARHELIA_CURSOR_DC_ENABLE 0x00000016
#define PARHELIA_CURSOR_UPDATE_STATUS 0x0450
#define PARHELIA_CURSOR_UPDATE_READY BIT(22)

struct parhelia_cursor {
	struct drm_plane plane;
	struct parhelia_device *pdev;
	struct drm_gem_vram_object *image;
	struct iosys_map image_map;
	u32 image_offset;
};

static inline struct parhelia_cursor *
to_parhelia_cursor(struct drm_plane *plane)
{
	return container_of(plane, struct parhelia_cursor, plane);
}

static unsigned int parhelia_cursor_head(struct parhelia_device *pdev)
{
	if (pdev->num_outputs && pdev->outputs[0].desc)
		return pdev->outputs[0].desc->preferred_head;
	return PARHELIA_DC_HEAD0;
}

static u32 parhelia_cursor_reg(struct parhelia_cursor *cursor, u32 reg)
{
	return PARHELIA_CURSOR_BASE(parhelia_cursor_head(cursor->pdev)) + reg;
}

static void parhelia_cursor_wait_ready(struct parhelia_cursor *cursor)
{
	u32 status;

	/*
	 * Vendor cursor moves wait for the display latch. Keep the same order,
	 * but bound the wait if the controller has stopped.
	 */
	readl_poll_timeout(cursor->pdev->mmio + PARHELIA_CURSOR_UPDATE_STATUS,
			   status, status & PARHELIA_CURSOR_UPDATE_READY, 5,
			   20000);
}

static unsigned int parhelia_cursor_palette_index(u32 *palette,
						  unsigned int *count, u32 rgb)
{
	unsigned int i, best = 0;
	u32 best_distance = U32_MAX;

	for (i = 0; i < *count; i++)
		if (palette[i] == rgb)
			return i;
	if (*count < 16) {
		palette[*count] = rgb;
		return (*count)++;
	}

	for (i = 0; i < 16; i++) {
		int dr = ((rgb >> 16) & 0xff) - ((palette[i] >> 16) & 0xff);
		int dg = ((rgb >> 8) & 0xff) - ((palette[i] >> 8) & 0xff);
		int db = (rgb & 0xff) - (palette[i] & 0xff);
		u32 distance = dr * dr + dg * dg + db * db;

		if (distance < best_distance) {
			best_distance = distance;
			best = i;
		}
	}
	return best;
}

static void parhelia_cursor_upload(struct parhelia_cursor *cursor,
				   const struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_object *gem;
	struct iosys_map source_map = {};
	u32 palette[16] = { 0 };
	unsigned int palette_count = 0;
	u32 row[PARHELIA_CURSOR_STRIDE / sizeof(u32)];
	u32 *pixels;
	unsigned int x, y, bitplane, i;
	int ret;

	pixels = kcalloc(PARHELIA_CURSOR_WIDTH * PARHELIA_CURSOR_HEIGHT,
			 sizeof(*pixels), GFP_KERNEL);
	if (!pixels)
		return;

	gem = drm_gem_fb_get_obj(fb, 0);
	if (!gem)
		goto out_free;
	ret = drm_gem_vmap(gem, &source_map);
	if (ret)
		goto out_free;
	for (y = 0; y < fb->height; y++)
		iosys_map_memcpy_from(&pixels[y * PARHELIA_CURSOR_WIDTH],
				      &source_map, (size_t)y * fb->pitches[0],
				      fb->width * sizeof(u32));
	drm_gem_vunmap(gem, &source_map);

	for (i = 0; i < PARHELIA_CURSOR_WIDTH * PARHELIA_CURSOR_HEIGHT; i++)
		if (pixels[i] >> 24)
			parhelia_cursor_palette_index(palette, &palette_count,
						      pixels[i] & 0x00ffffff);
	if (!palette_count)
		palette_count = 1;

	for (y = 0; y < PARHELIA_CURSOR_HEIGHT; y++) {
		memset(row, 0, sizeof(row));
		for (x = 0; x < PARHELIA_CURSOR_WIDTH; x++) {
			u32 pixel = pixels[y * PARHELIA_CURSOR_WIDTH + x];
			u32 alpha = pixel >> 24;
			unsigned int index =
				alpha ? parhelia_cursor_palette_index(
						palette, &palette_count,
						pixel & 0x00ffffff) :
					0;
			unsigned int word = x / 32;
			u32 bit = BIT(x % 32);

			for (bitplane = 0; bitplane < 4; bitplane++)
				if (index & BIT(bitplane))
					row[bitplane * 2 + word] |= bit;
			/* Planes 4..8 carry source alpha bits 3..7. */
			for (bitplane = 0; bitplane < 5; bitplane++)
				if (alpha & BIT(bitplane + 3))
					row[8 + bitplane * 2 + word] |= bit;
		}
		iosys_map_memcpy_to(&cursor->image_map,
				    y * PARHELIA_CURSOR_STRIDE, row,
				    sizeof(row));
	}
	wmb(); /* Make the complete planar image visible before changing its base. */

	parhelia_write(cursor->pdev,
		       parhelia_cursor_reg(cursor, PARHELIA_CURSOR_ADDR),
		       cursor->image_offset);
	parhelia_write(cursor->pdev,
		       parhelia_cursor_reg(cursor, PARHELIA_CURSOR_CFG),
		       PARHELIA_CURSOR_CFG_16COLOR);
	for (i = 0; i < palette_count; i++) {
		parhelia_write(cursor->pdev,
			       parhelia_cursor_reg(cursor, PARHELIA_CURSOR_CTL),
			       (i << 8) | PARHELIA_CURSOR_CTL_16COLOR);
		parhelia_write(cursor->pdev,
			       parhelia_cursor_reg(cursor,
						   PARHELIA_CURSOR_COLOR),
			       palette[i]);
	}
out_free:
	kfree(pixels);
}

static int parhelia_cursor_atomic_check(struct drm_plane *plane,
					struct drm_atomic_commit *commit)
{
	struct drm_plane_state *state =
		drm_atomic_get_new_plane_state(commit, plane);
	struct drm_crtc_state *crtc_state = NULL;
	int ret;

	if (state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(commit, state->crtc);
	ret = drm_atomic_helper_check_plane_state(state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, true,
						  true);
	if (ret || !state->visible)
		return ret;
	if (state->fb->width > PARHELIA_CURSOR_WIDTH ||
	    state->fb->height > PARHELIA_CURSOR_HEIGHT || state->src_x ||
	    state->src_y || state->src_w != state->fb->width << 16 ||
	    state->src_h != state->fb->height << 16)
		return -EINVAL;
	return 0;
}

static void parhelia_cursor_atomic_update(struct drm_plane *plane,
					  struct drm_atomic_commit *commit)
{
	struct parhelia_cursor *cursor = to_parhelia_cursor(plane);
	struct drm_plane_state *state =
		drm_atomic_get_new_plane_state(commit, plane);
	struct parhelia_device *pdev = cursor->pdev;
	unsigned int head = parhelia_cursor_head(pdev);
	int x = max_t(int, state->crtc_x, -PARHELIA_CURSOR_WIDTH) + 64;
	int y = max_t(int, state->crtc_y, -PARHELIA_CURSOR_HEIGHT) + 64;
	u32 dc;

	/* The legacy cursor ioctl reuses one framebuffer and changes its pixels
	 * in place, so a framebuffer pointer comparison cannot detect updates.
	 */
	parhelia_cursor_upload(cursor, state);

	parhelia_cursor_wait_ready(cursor);
	parhelia_write(pdev, parhelia_cursor_reg(cursor, PARHELIA_CURSOR_POS),
		       ((x & 0x7fff) << 14) | (y & 0x7fff));
	dc = parhelia_read(pdev, PARHELIA_DC(head) + PARHELIA_DC_PARAMS);
	parhelia_write(pdev, PARHELIA_DC(head) + PARHELIA_DC_PARAMS,
		       dc | PARHELIA_CURSOR_DC_ENABLE);
	parhelia_write(pdev,
		       PARHELIA_CURSOR_BASE(head ^ 1) + PARHELIA_CURSOR_CTL, 0);
	parhelia_write(pdev, parhelia_cursor_reg(cursor, PARHELIA_CURSOR_CTL),
		       PARHELIA_CURSOR_CTL_16COLOR);
}

static void parhelia_cursor_atomic_disable(struct drm_plane *plane,
					   struct drm_atomic_commit *commit)
{
	struct parhelia_cursor *cursor = to_parhelia_cursor(plane);
	struct parhelia_device *pdev = cursor->pdev;
	unsigned int head = parhelia_cursor_head(pdev);
	u32 dc;

	parhelia_write(pdev, parhelia_cursor_reg(cursor, PARHELIA_CURSOR_CTL),
		       0);
	dc = parhelia_read(pdev, PARHELIA_DC(head) + PARHELIA_DC_PARAMS);
	parhelia_write(pdev, PARHELIA_DC(head) + PARHELIA_DC_PARAMS,
		       dc & ~PARHELIA_CURSOR_DC_ENABLE);
}

static const struct drm_plane_helper_funcs parhelia_cursor_helper_funcs = {
	.prepare_fb = drm_gem_vram_plane_helper_prepare_fb,
	.cleanup_fb = drm_gem_vram_plane_helper_cleanup_fb,
	.atomic_check = parhelia_cursor_atomic_check,
	.atomic_update = parhelia_cursor_atomic_update,
	.atomic_disable = parhelia_cursor_atomic_disable,
};

static const struct drm_plane_funcs parhelia_cursor_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static void parhelia_cursor_release(struct drm_device *drm, void *res)
{
	struct parhelia_cursor *cursor = res;

	if (!ttm_bo_reserve(&cursor->image->bo, false, false, NULL)) {
		drm_gem_vram_vunmap(cursor->image, &cursor->image_map);
		ttm_bo_unreserve(&cursor->image->bo);
	}
	parhelia_vram_unpin(cursor->image);
	drm_gem_vram_put(cursor->image);
}

int parhelia_cursor_init(struct parhelia_device *pdev)
{
	static const u32 formats[] = { DRM_FORMAT_ARGB8888 };
	struct parhelia_cursor *cursor;
	struct drm_gem_vram_object *image;
	struct iosys_map image_map = {};
	u32 address;
	int ret;

	cursor = drmm_universal_plane_alloc(&pdev->drm, struct parhelia_cursor,
					    plane,
					    drm_crtc_mask(&pdev->pipe.crtc),
					    &parhelia_cursor_funcs, formats,
					    ARRAY_SIZE(formats), NULL,
					    DRM_PLANE_TYPE_CURSOR, NULL);
	if (IS_ERR(cursor))
		return PTR_ERR(cursor);
	cursor->pdev = pdev;

	image = drm_gem_vram_create(&pdev->drm, PARHELIA_CURSOR_SIZE, 1);
	if (IS_ERR(image))
		return PTR_ERR(image);
	ret = parhelia_vram_pin(image);
	if (ret)
		goto err_put;
	ret = ttm_bo_reserve(&image->bo, true, false, NULL);
	if (ret)
		goto err_unpin;
	ret = drm_gem_vram_vmap(image, &image_map);
	ttm_bo_unreserve(&image->bo);
	if (ret)
		goto err_unpin;
	ret = parhelia_vram_gpu_addr(pdev, image, 0, PARHELIA_CURSOR_SIZE,
				     &address);
	if (ret)
		goto err_unmap;
	cursor->image = image;
	cursor->image_map = image_map;
	cursor->image_offset = address;
	iosys_map_memset(&cursor->image_map, 0, 0, PARHELIA_CURSOR_SIZE);

	ret = drmm_add_action_or_reset(&pdev->drm, parhelia_cursor_release,
				       cursor);
	if (ret)
		return ret;
	drm_plane_helper_add(&cursor->plane, &parhelia_cursor_helper_funcs);
	pdev->pipe.crtc.cursor = &cursor->plane;
	pdev->drm.mode_config.cursor_width = PARHELIA_CURSOR_WIDTH;
	pdev->drm.mode_config.cursor_height = PARHELIA_CURSOR_HEIGHT;
	return 0;

err_unmap:
	if (!ttm_bo_reserve(&image->bo, false, false, NULL)) {
		drm_gem_vram_vunmap(image, &image_map);
		ttm_bo_unreserve(&image->bo);
	}
err_unpin:
	parhelia_vram_unpin(image);
err_put:
	drm_gem_vram_put(image);
	return ret;
}
