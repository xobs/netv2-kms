/*
 * SimpleDRM firmware framebuffer driver
 * Copyright (c) 2012-2014 David Herrmann <dh.herrmann@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <asm/unaligned.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <linux/dma-buf.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "simpledrm.h"

static inline void sdrm_put(u8 *dst, u32 four_cc, u16 r, u16 g, u16 b)
{
	switch (four_cc) {
	case DRM_FORMAT_RGB565:
		r >>= 11;
		g >>= 10;
		b >>= 11;
		put_unaligned((u16)((r << 11) | (g << 5) | b), (u16 *)dst);
		break;
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ARGB1555:
		r >>= 11;
		g >>= 11;
		b >>= 11;
		put_unaligned((u16)((r << 10) | (g << 5) | b), (u16 *)dst);
		break;
	case DRM_FORMAT_RGB888:
		r >>= 8;
		g >>= 8;
		b >>= 8;
#ifdef __LITTLE_ENDIAN
		dst[2] = r;
		dst[1] = g;
		dst[0] = b;
#elif defined(__BIG_ENDIAN)
		dst[0] = r;
		dst[1] = g;
		dst[2] = b;
#endif
		break;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		r >>= 8;
		g >>= 8;
		b >>= 8;
		put_unaligned((u32)((r << 16) | (g << 8) | b), (u32 *)dst);
		break;
	case DRM_FORMAT_ABGR8888:
		r >>= 8;
		g >>= 8;
		b >>= 8;
		put_unaligned((u32)((b << 16) | (g << 8) | r), (u32 *)dst);
		break;
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		r >>= 4;
		g >>= 4;
		b >>= 4;
		put_unaligned((u32)((r << 20) | (g << 10) | b), (u32 *)dst);
		break;
	}
}

static void sdrm_blit_from_xrgb8888(const u8 *src, u32 src_stride, u32 src_bpp,
				    u8 *dst, u32 dst_stride, u32 dst_bpp,
				    u32 dst_four_cc, u32 width, u32 height)
{
	u32 val, i;

	while (height--) {
		for (i = 0; i < width; ++i) {
			val = get_unaligned((const u32 *)&src[i * src_bpp]);
			sdrm_put(&dst[i * dst_bpp], dst_four_cc,
				 (val & 0x00ff0000U) >> 8,
				 (val & 0x0000ff00U),
				 (val & 0x000000ffU) << 8);
		}

		src += src_stride;
		dst += dst_stride;
	}
}

static void sdrm_blit_from_rgb565(const u8 *src, u32 src_stride, u32 src_bpp,
				  u8 *dst, u32 dst_stride, u32 dst_bpp,
				  u32 dst_four_cc, u32 width, u32 height)
{
	u32 val, i;

	while (height--) {
		for (i = 0; i < width; ++i) {
			val = get_unaligned((const u16 *)&src[i * src_bpp]);
			sdrm_put(&dst[i * dst_bpp], dst_four_cc,
				 (val & 0xf800),
				 (val & 0x07e0) << 5,
				 (val & 0x001f) << 11);
		}

		src += src_stride;
		dst += dst_stride;
	}
}

static void sdrm_blit_lines(const u8 *src, u32 src_stride,
			    u8 *dst, u32 dst_stride,
			    u32 bpp, u32 width, u32 height)
{
	u32 len;

	len = width * bpp;

	while (height--) {
		memcpy(dst, src, len);
		src += src_stride;
		dst += dst_stride;
	}
}

static void sdrm_blit(struct sdrm_framebuffer *sfb, u32 x, u32 y,
		      u32 width, u32 height)
{
	struct drm_framebuffer *fb = &sfb->base;
	struct drm_device *ddev = fb->dev;
	struct sdrm_device *sdrm = ddev->dev_private;
	u32 src_bpp, dst_bpp, x2, y2;
	u8 *src, *dst;

	/* already unmapped; ongoing handover? */
	if (!sdrm->fb_map)
		return;

	/* empty dirty-region, nothing to do */
	if (!width || !height)
		return;
	if (x >= fb->width || y >= fb->height)
		return;

	/* sanity checks */
	if (x + width < x)
		width = fb->width - x;
	if (y + height < y)
		height = fb->height - y;

	/* get intersection of dirty region and scan-out region */
	x2 = min(x + width, sdrm->fb_width);
	y2 = min(y + height, sdrm->fb_height);
	if (x2 <= x || y2 <= y)
		return;
	width = x2 - x;
	height = y2 - y;

	/* get buffer offsets */
	src = sfb->obj->vmapping;
	dst = sdrm->fb_map;

	/* bo is guaranteed to be big enough; size checks not needed */
	src_bpp = (fb->bits_per_pixel + 7) / 8;
	src += fb->offsets[0] + y * fb->pitches[0] + x * src_bpp;

	dst_bpp = (sdrm->fb_bpp + 7) / 8;
	dst += y * sdrm->fb_stride + x * dst_bpp;

	/* if formats are identical, do a line-by-line copy.. */
	if (fb->pixel_format == sdrm->fb_format) {
		sdrm_blit_lines(src, fb->pitches[0],
				dst, sdrm->fb_stride,
				src_bpp, width, height);
		return;
	}

	/* ..otherwise call slow blit-function */
	switch (fb->pixel_format) {
	case DRM_FORMAT_ARGB8888:
		/* fallthrough */
	case DRM_FORMAT_XRGB8888:
		sdrm_blit_from_xrgb8888(src, fb->pitches[0], src_bpp,
					dst, sdrm->fb_stride, dst_bpp,
					sdrm->fb_format, width, height);
		break;
	case DRM_FORMAT_RGB565:
		sdrm_blit_from_rgb565(src, fb->pitches[0], src_bpp,
				      dst, sdrm->fb_stride, dst_bpp,
				      sdrm->fb_format, width, height);
		break;
	}
}

static int sdrm_begin_access(struct sdrm_framebuffer *sfb)
{
	int r;

	r = sdrm_gem_get_pages(sfb->obj);
	if (r)
		return r;

	if (!sfb->obj->base.import_attach)
		return 0;

	return dma_buf_begin_cpu_access(sfb->obj->base.import_attach->dmabuf,
					DMA_FROM_DEVICE);
}

static void sdrm_end_access(struct sdrm_framebuffer *sfb)
{
	if (!sfb->obj->base.import_attach)
		return;

	dma_buf_end_cpu_access(sfb->obj->base.import_attach->dmabuf,
			       DMA_FROM_DEVICE);
}

int sdrm_dirty(struct drm_framebuffer *fb,
	       struct drm_file *file,
	       unsigned int flags, unsigned int color,
	       struct drm_clip_rect *clips,
	       unsigned int num_clips)
{
	struct sdrm_framebuffer *sfb = to_sdrm_fb(fb);
	struct drm_device *ddev = fb->dev;
	struct sdrm_device *sdrm = ddev->dev_private;
	struct drm_clip_rect full_clip;
	unsigned int i;
	int r;

	if (!clips || !num_clips) {
		full_clip.x1 = 0;
		full_clip.x2 = fb->width;
		full_clip.y1 = 0;
		full_clip.y2 = fb->height;
		clips = &full_clip;
		num_clips = 1;
	}

	drm_modeset_lock_all(ddev);

	if (sdrm->plane.fb != fb) {
		r = 0;
		goto unlock;
	}

	r = sdrm_begin_access(sfb);
	if (r)
		goto unlock;

	for (i = 0; i < num_clips; i++) {
		if (clips[i].x2 <= clips[i].x1 ||
		    clips[i].y2 <= clips[i].y1)
			continue;

		sdrm_blit(sfb, clips[i].x1, clips[i].y1,
			  clips[i].x2 - clips[i].x1,
			  clips[i].y2 - clips[i].y1);
	}

	sdrm_end_access(sfb);

unlock:
	drm_modeset_unlock_all(ddev);
	return 0;
}

int sdrm_dirty_all_locked(struct sdrm_device *sdrm)
{
	struct drm_framebuffer *fb;
	struct sdrm_framebuffer *sfb;
	int r;

	fb = sdrm->plane.fb;
	if (!fb)
		return 0;

	sfb = to_sdrm_fb(fb);
	r = sdrm_begin_access(sfb);
	if (r)
		return r;

	sdrm_blit(sfb, 0, 0, fb->width, fb->height);

	sdrm_end_access(sfb);

	return 0;
}

int sdrm_dirty_all_unlocked(struct sdrm_device *sdrm)
{
	int r;

	drm_modeset_lock_all(sdrm->ddev);
	r = sdrm_dirty_all_locked(sdrm);
	drm_modeset_unlock_all(sdrm->ddev);

	return r;
}
