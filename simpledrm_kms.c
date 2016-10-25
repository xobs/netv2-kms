/*
 * SimpleDRM firmware framebuffer driver
 * Copyright (c) 2012-2014 David Herrmann <dh.herrmann@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_gem.h>
#include <drm/drm_simple_kms_helper.h>
#include <linux/slab.h>

#include "simpledrm.h"

static const uint32_t sdrm_formats[] = {
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_XRGB8888,
};

void sdrm_lastclose(struct drm_device *ddev)
{
	struct sdrm_device *sdrm = ddev->dev_private;

	sdrm_fbdev_restore_mode(sdrm);
}

static int sdrm_conn_get_modes(struct drm_connector *conn)
{
	struct sdrm_device *sdrm = conn->dev->dev_private;
	struct drm_display_mode *mode;

	mode = drm_cvt_mode(sdrm->ddev, sdrm->fb_width, sdrm->fb_height,
			    60, false, false, false);
	if (!mode)
		return 0;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(conn, mode);

	return 1;
}

static int sdrm_conn_mode_valid(struct drm_connector *connector,
				struct drm_display_mode *mode)
{
	return MODE_OK;
}

static const struct drm_connector_helper_funcs sdrm_conn_hfuncs = {
	.get_modes = sdrm_conn_get_modes,
	.best_encoder = drm_atomic_helper_best_encoder,
	.mode_valid = sdrm_conn_mode_valid,
};

static enum drm_connector_status sdrm_conn_detect(struct drm_connector *conn,
						  bool force)
{
	/*
	 * We simulate an always connected monitor. simple-fb doesn't
	 * provide any way to detect whether the connector is active. Hence,
	 * signal DRM core that it is always connected.
	 */

	return connector_status_connected;
}

static const struct drm_connector_funcs sdrm_conn_ops = {
	.dpms = drm_atomic_helper_connector_dpms,
	.reset = drm_atomic_helper_connector_reset,
	.detect = sdrm_conn_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void sdrm_crtc_send_vblank_event(struct drm_crtc *crtc)
{
	if (crtc->state && crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);
		crtc->state->event = NULL;
	}
}

void netv_display_pipe_update(struct sdrm_device *netv,
			      struct drm_plane_state *plane_state)
{
	struct drm_framebuffer *fb = netv->plane.state->fb;

	sdrm_crtc_send_vblank_event(&netv->crtc);
	sdrm_fbdev_display_pipe_update(netv, fb);

	if (fb && fb->funcs->dirty) {
		netv->plane.fb = fb;
		sdrm_dirty_all_locked(netv);
	}
}

static void netv_display_pipe_enable(struct sdrm_device *netv,
				     struct drm_crtc_state *crtc_state)
{
	sdrm_crtc_send_vblank_event(&netv->crtc);
}

static void netv_display_pipe_disable(struct sdrm_device *netv)
{
	sdrm_crtc_send_vblank_event(&netv->crtc);
}

static const struct netv_display_pipe_funcs sdrm_pipe_funcs = {
	.update = netv_display_pipe_update,
	.enable = netv_display_pipe_enable,
	.disable = netv_display_pipe_disable,
};

static int sdrm_fb_create_handle(struct drm_framebuffer *fb,
				 struct drm_file *dfile,
				 unsigned int *handle)
{
	struct sdrm_framebuffer *sfb = to_sdrm_fb(fb);

	return drm_gem_handle_create(dfile, &sfb->obj->base, handle);
}

static void sdrm_fb_destroy(struct drm_framebuffer *fb)
{
	struct sdrm_framebuffer *sfb = to_sdrm_fb(fb);

	drm_framebuffer_cleanup(fb);
	drm_gem_object_unreference_unlocked(&sfb->obj->base);
	kfree(sfb);
}

static const struct drm_framebuffer_funcs sdrm_fb_ops = {
	.create_handle = sdrm_fb_create_handle,
	.dirty = sdrm_dirty,
	.destroy = sdrm_fb_destroy,
};

static struct drm_framebuffer *sdrm_fb_create(struct drm_device *ddev,
					      struct drm_file *dfile,
					      const struct drm_mode_fb_cmd2 *cmd)
{
	struct sdrm_framebuffer *fb;
	struct drm_gem_object *gobj;
	u32 bpp, size;
	int ret;
	void *err;

	if (cmd->flags)
		return ERR_PTR(-EINVAL);

	gobj = drm_gem_object_lookup(dfile, cmd->handles[0]);
	if (!gobj)
		return ERR_PTR(-EINVAL);

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb) {
		err = ERR_PTR(-ENOMEM);
		goto err_unref;
	}
	fb->obj = to_sdrm_bo(gobj);

	fb->base.pitches[0] = cmd->pitches[0];
	fb->base.offsets[0] = cmd->offsets[0];
	fb->base.width = cmd->width;
	fb->base.height = cmd->height;
	fb->base.pixel_format = cmd->pixel_format;
	drm_fb_get_bpp_depth(cmd->pixel_format, &fb->base.depth,
			     &fb->base.bits_per_pixel);

	/*
	 * width/height are already clamped into min/max_width/height range,
	 * so overflows are not possible
	 */

	bpp = (fb->base.bits_per_pixel + 7) / 8;
	size = cmd->pitches[0] * cmd->height;
	if (!bpp ||
	    bpp > 4 ||
	    cmd->pitches[0] < bpp * fb->base.width ||
	    cmd->pitches[0] > 0xffffU ||
	    size + fb->base.offsets[0] < size ||
	    size + fb->base.offsets[0] > fb->obj->base.size) {
		err = ERR_PTR(-EINVAL);
		goto err_free;
	}

	ret = drm_framebuffer_init(ddev, &fb->base, &sdrm_fb_ops);
	if (ret < 0) {
		err = ERR_PTR(ret);
		goto err_free;
	}

	DRM_DEBUG_KMS("[FB:%d] pixel_format: %s\n", fb->base.base.id,
		      drm_get_format_name(fb->base.pixel_format));

	return &fb->base;

err_free:
	kfree(fb);
err_unref:
	drm_gem_object_unreference_unlocked(gobj);

	return err;
}

static const struct drm_mode_config_funcs sdrm_mode_config_ops = {
	.fb_create = sdrm_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

int sdrm_drm_modeset_init(struct sdrm_device *sdrm)
{
	struct drm_connector *conn = &sdrm->connector;
	struct drm_device *ddev = sdrm->ddev;
	int ret;

	drm_mode_config_init(ddev);
	ddev->mode_config.min_width = sdrm->fb_width;
	ddev->mode_config.max_width = sdrm->fb_width;
	ddev->mode_config.min_height = sdrm->fb_height;
	ddev->mode_config.max_height = sdrm->fb_height;
	ddev->mode_config.preferred_depth = sdrm->fb_bpp;
	ddev->mode_config.funcs = &sdrm_mode_config_ops;

	drm_connector_helper_add(conn, &sdrm_conn_hfuncs);
	ret = drm_connector_init(ddev, conn, &sdrm_conn_ops,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		goto err_cleanup;

	ret = drm_mode_create_dirty_info_property(ddev);
	if (ret)
		goto err_cleanup;

	drm_object_attach_property(&conn->base,
				   ddev->mode_config.dirty_info_property,
				   DRM_MODE_DIRTY_ON);

	ret = netv_simple_display_pipe_init(ddev, sdrm, &sdrm_pipe_funcs,
					   sdrm_formats,
					   ARRAY_SIZE(sdrm_formats), conn);
	if (ret)
		goto err_cleanup;

	drm_mode_config_reset(ddev);

	return 0;

err_cleanup:
	drm_mode_config_cleanup(ddev);

	return ret;
}
