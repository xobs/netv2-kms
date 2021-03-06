/*
 * SimpleDRM firmware framebuffer driver
 * Copyright (c) 2012-2014 David Herrmann <dh.herrmann@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef SDRM_DRV_H
#define SDRM_DRV_H

#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_gem.h>

struct simplefb_format;
struct sdrm_device;

struct netv_display_pipe_funcs {
	void (*enable)(struct sdrm_device *netv,
		       struct drm_crtc_state *crtc_state);
	void (*disable)(struct sdrm_device *netv);
	int (*check)(struct sdrm_device *netv,
		     struct drm_plane_state *plane_state,
		     struct drm_crtc_state *crtc_state);
	void (*update)(struct sdrm_device *netv,
		       struct drm_plane_state *plane_state);
};

struct sdrm_device {
	struct drm_device *ddev;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_plane plane;
	struct drm_connector connector;
	struct sdrm_fbdev *fbdev;

	/* framebuffer information */
	const struct simplefb_format *fb_sformat;
	u32 fb_format;
	u32 fb_width;
	u32 fb_height;
	u32 fb_stride;
	u32 fb_bpp;
	unsigned long fb_base;
	unsigned long fb_size;
	void *fb_map;

	const struct netv_display_pipe_funcs *funcs;
};

void sdrm_lastclose(struct drm_device *ddev);
int sdrm_drm_modeset_init(struct sdrm_device *sdrm);
int sdrm_drm_mmap(struct file *filp, struct vm_area_struct *vma);

int sdrm_dirty(struct drm_framebuffer *fb,
	       struct drm_file *file,
	       unsigned int flags, unsigned int color,
	       struct drm_clip_rect *clips,
	       unsigned int num_clips);
int sdrm_dirty_all_locked(struct sdrm_device *sdrm);
int sdrm_dirty_all_unlocked(struct sdrm_device *sdrm);

struct sdrm_gem_object {
	struct drm_gem_object base;
	struct sg_table *sg;
	struct page **pages;
	void *vmapping;
};

#define to_sdrm_bo(x) container_of(x, struct sdrm_gem_object, base)

struct sdrm_gem_object *sdrm_gem_alloc_object(struct drm_device *ddev,
					      size_t size);
struct drm_gem_object *sdrm_gem_prime_import(struct drm_device *ddev,
					     struct dma_buf *dma_buf);
void sdrm_gem_free_object(struct drm_gem_object *obj);
int sdrm_gem_get_pages(struct sdrm_gem_object *obj);

int sdrm_dumb_create(struct drm_file *file_priv, struct drm_device *ddev,
		     struct drm_mode_create_dumb *arg);
int sdrm_dumb_destroy(struct drm_file *file_priv, struct drm_device *ddev,
		      uint32_t handle);
int sdrm_dumb_map_offset(struct drm_file *file_priv, struct drm_device *ddev,
			 uint32_t handle, uint64_t *offset);

struct sdrm_framebuffer {
	struct drm_framebuffer base;
	struct sdrm_gem_object *obj;
};

int netv_simple_display_pipe_init(struct drm_device *dev,
                        struct sdrm_device *pipe,
                        const struct netv_display_pipe_funcs *funcs,
                        const uint32_t *formats, unsigned int format_count,
                        struct drm_connector *connector);

#define to_sdrm_fb(x) container_of(x, struct sdrm_framebuffer, base)

#ifdef CONFIG_FB

void sdrm_fbdev_init(struct sdrm_device *sdrm);
void sdrm_fbdev_cleanup(struct sdrm_device *sdrm);
void sdrm_fbdev_display_pipe_update(struct sdrm_device *sdrm,
				    struct drm_framebuffer *fb);
void sdrm_fbdev_restore_mode(struct sdrm_device *sdrm);
void sdrm_fbdev_kickout_init(void);
void sdrm_fbdev_kickout_exit(void);

#else

static inline void sdrm_fbdev_init(struct sdrm_device *sdrm)
{
}

static inline void sdrm_fbdev_cleanup(struct sdrm_device *sdrm)
{
}

static inline void sdrm_fbdev_display_pipe_update(struct sdrm_device *sdrm,
						  struct drm_framebuffer *fb)
{
}

static inline void sdrm_fbdev_restore_mode(struct sdrm_device *sdrm)
{
}

static inline void sdrm_fbdev_kickout_init(void)
{
}

static inline void sdrm_fbdev_kickout_exit(void)
{
}
#endif

#endif /* SDRM_DRV_H */
