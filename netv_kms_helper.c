/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane_helper.h>
#include <linux/slab.h>
#include "simpledrm.h"

/**
 * DOC: overview
 *
 * This helper library provides helpers for drivers for simple display
 * hardware.
 *
 * netv_display_pipe_init() initializes a simple display pipeline
 * which has only one full-screen scanout buffer feeding one output. The
 * pipeline is represented by struct &netv_display_pipe and binds
 * together &drm_plane, &drm_crtc and &drm_encoder structures into one fixed
 * entity. Some flexibility for code reuse is provided through a separately
 * allocated &drm_connector object and supporting optional &drm_bridge
 * encoder drivers.
 */

static void netv_encoder_mode_set(struct drm_encoder *encoder,
				  struct drm_display_mode *mode,
				  struct drm_display_mode *adjusted_mode)
{
}

static void netv_encoder_dpms(struct drm_encoder *encoder, int state)
{
}

static void netv_encoder_prepare(struct drm_encoder *encoder)
{
}

static void netv_encoder_commit(struct drm_encoder *encoder)
{
}

static const struct drm_encoder_helper_funcs netv_kms_encoder_helper_funcs = {
	.dpms = netv_encoder_dpms,
	.mode_set = netv_encoder_mode_set,
	.prepare = netv_encoder_prepare,
	.commit = netv_encoder_commit,
};

static const struct drm_encoder_funcs netv_kms_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static void netv_kms_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	switch (mode) {
	case DRM_MODE_DPMS_ON:
	case DRM_MODE_DPMS_STANDBY:
	case DRM_MODE_DPMS_SUSPEND:
	case DRM_MODE_DPMS_OFF:
	default:
		return;
	}
}

static void netv_kms_crtc_enable(struct drm_crtc *crtc)
{
	struct sdrm_device *pipe;

	pipe = container_of(crtc, struct sdrm_device, crtc);
	if (!pipe->funcs || !pipe->funcs->enable)
		return;

	pipe->funcs->enable(pipe, crtc->state);
}

static void netv_kms_crtc_disable(struct drm_crtc *crtc)
{
	struct sdrm_device *pipe;

	pipe = container_of(crtc, struct sdrm_device, crtc);
	if (!pipe->funcs || !pipe->funcs->disable)
		return;

	pipe->funcs->disable(pipe);
}

static const struct drm_crtc_helper_funcs netv_kms_crtc_helper_funcs = {
	.dpms = netv_kms_crtc_dpms,
	.disable = netv_kms_crtc_disable,
	.enable = netv_kms_crtc_enable,
};

static const struct drm_crtc_funcs netv_kms_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static int netv_kms_plane_atomic_check(struct drm_plane *plane,
					struct drm_plane_state *plane_state)
{
	struct drm_rect src = {
		.x1 = plane_state->src_x,
		.y1 = plane_state->src_y,
		.x2 = plane_state->src_x + plane_state->src_w,
		.y2 = plane_state->src_y + plane_state->src_h,
	};
	struct drm_rect dest = {
		.x1 = plane_state->crtc_x,
		.y1 = plane_state->crtc_y,
		.x2 = plane_state->crtc_x + plane_state->crtc_w,
		.y2 = plane_state->crtc_y + plane_state->crtc_h,
	};
	struct drm_rect clip = { 0 };
	struct sdrm_device *pipe;
	struct drm_crtc_state *crtc_state;
	bool visible;
	int ret;

	pipe = container_of(plane, struct sdrm_device, plane);
	crtc_state = drm_atomic_get_existing_crtc_state(plane_state->state,
							&pipe->crtc);
	if (crtc_state->enable != !!plane_state->crtc)
		return -EINVAL; /* plane must match crtc enable state */

	if (!crtc_state->enable)
		return 0; /* nothing to check when disabling or disabled */

	clip.x2 = crtc_state->adjusted_mode.hdisplay;
	clip.y2 = crtc_state->adjusted_mode.vdisplay;
	ret = drm_plane_helper_check_update(plane, &pipe->crtc,
					    plane_state->fb,
					    &src, &dest, &clip,
					    plane_state->rotation,
					    DRM_PLANE_HELPER_NO_SCALING,
					    DRM_PLANE_HELPER_NO_SCALING,
					    false, true, &visible);
	if (ret)
		return ret;

	if (!visible)
		return -EINVAL;

	if (!pipe->funcs || !pipe->funcs->check)
		return 0;

	return pipe->funcs->check(pipe, plane_state, crtc_state);
}

static void netv_kms_plane_atomic_update(struct drm_plane *plane,
					struct drm_plane_state *pstate)
{
	struct sdrm_device *pipe;

	pipe = container_of(plane, struct sdrm_device, plane);
	if (!pipe->funcs || !pipe->funcs->update)
		return;

	pipe->funcs->update(pipe, pstate);
}

static const struct drm_plane_helper_funcs netv_kms_plane_helper_funcs = {
	.atomic_check = netv_kms_plane_atomic_check,
	.atomic_update = netv_kms_plane_atomic_update,
};

static const struct drm_plane_funcs netv_kms_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

/**
 * netv_display_pipe_init - Initialize a simple display pipeline
 * @dev: DRM device
 * @pipe: simple display pipe object to initialize
 * @funcs: callbacks for the display pipe (optional)
 * @formats: array of supported formats (%DRM_FORMAT_*)
 * @format_count: number of elements in @formats
 * @connector: connector to attach and register
 *
 * Sets up a display pipeline which consist of a really simple
 * plane-crtc-encoder pipe coupled with the provided connector.
 * Teardown of a simple display pipe is all handled automatically by the drm
 * core through calling drm_mode_config_cleanup(). Drivers afterwards need to
 * release the memory for the structure themselves.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int netv_simple_display_pipe_init(struct drm_device *dev,
			struct sdrm_device *netv,
			const struct netv_display_pipe_funcs *funcs,
			const uint32_t *formats, unsigned int format_count,
			struct drm_connector *connector)
{
	struct drm_encoder *encoder = &netv->encoder;
	struct drm_plane *plane = &netv->plane;
	struct drm_crtc *crtc = &netv->crtc;
	int ret;

	netv->funcs = funcs;

	drm_plane_helper_add(plane, &netv_kms_plane_helper_funcs);
	ret = drm_universal_plane_init(dev, plane, 0,
				       &netv_kms_plane_funcs,
				       formats, format_count,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_crtc_helper_add(crtc, &netv_kms_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(dev, crtc, plane, NULL,
					&netv_kms_crtc_funcs, NULL);
	if (ret)
		return ret;

	encoder->possible_crtcs = 1 << drm_crtc_index(crtc);
	ret = drm_encoder_init(dev, encoder, &netv_kms_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	drm_encoder_helper_add(encoder, &netv_kms_encoder_helper_funcs);

	return drm_mode_connector_attach_encoder(connector, encoder);
}

MODULE_LICENSE("GPL");
