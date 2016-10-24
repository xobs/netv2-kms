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
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <linux/console.h>
#include <linux/fb.h>
#include <linux/platform_device.h>
#include <linux/platform_data/simplefb.h>

#include "simpledrm.h"

struct sdrm_fbdev {
	struct drm_fb_helper fb_helper;
	struct drm_framebuffer fb;
};

static inline struct sdrm_fbdev *to_sdrm_fbdev(struct drm_fb_helper *helper)
{
	return container_of(helper, struct sdrm_fbdev, fb_helper);
}

/*
 * Releasing has to be done outside the notifier callchain when we're
 * kicked out, since do_unregister_framebuffer() calls put_fb_info()
 * after the notifier has run.
 */
static void sdrm_fbdev_fb_destroy(struct fb_info *info)
{
	drm_fb_helper_release_fbi(info->par);
}

static struct fb_ops sdrm_fbdev_ops = {
	.owner		= THIS_MODULE,
	.fb_fillrect	= drm_fb_helper_cfb_fillrect,
	.fb_copyarea	= drm_fb_helper_cfb_copyarea,
	.fb_imageblit	= drm_fb_helper_cfb_imageblit,
	.fb_check_var	= drm_fb_helper_check_var,
	.fb_set_par	= drm_fb_helper_set_par,
	.fb_setcmap	= drm_fb_helper_setcmap,
	.fb_destroy	= sdrm_fbdev_fb_destroy,
};

static struct drm_framebuffer_funcs sdrm_fb_funcs = {
	.destroy = drm_framebuffer_cleanup,
};

static int sdrm_fbdev_create(struct drm_fb_helper *helper,
			     struct drm_fb_helper_surface_size *sizes)
{
	struct sdrm_fbdev *fbdev = to_sdrm_fbdev(helper);
	struct drm_device *ddev = helper->dev;
	struct sdrm_device *sdrm = ddev->dev_private;
	struct drm_framebuffer *fb = &fbdev->fb;
	struct drm_mode_fb_cmd2 mode_cmd = {
		.width = sdrm->fb_width,
		.height = sdrm->fb_height,
		.pitches[0] = sdrm->fb_stride,
		.pixel_format = sdrm->fb_format,
	};
	struct fb_info *fbi;
	int ret;

	fbi = drm_fb_helper_alloc_fbi(helper);
	if (IS_ERR(fbi))
		return PTR_ERR(fbi);

	drm_helper_mode_fill_fb_struct(fb, &mode_cmd);

	ret = drm_framebuffer_init(ddev, fb, &sdrm_fb_funcs);
	if (ret) {
		dev_err(ddev->dev, "Failed to initialize framebuffer: %d\n", ret);
		goto err_fb_info_destroy;
	}

	helper->fb = fb;
	fbi->par = helper;

	fbi->flags = FBINFO_DEFAULT | FBINFO_MISC_FIRMWARE |
		      FBINFO_CAN_FORCE_OUTPUT;
	fbi->fbops = &sdrm_fbdev_ops;

	drm_fb_helper_fill_fix(fbi, fb->pitches[0], fb->depth);
	drm_fb_helper_fill_var(fbi, helper, fb->width, fb->height);

	strncpy(fbi->fix.id, "simpledrmfb", 15);
	fbi->fix.smem_start = (unsigned long)sdrm->fb_base;
	fbi->fix.smem_len = sdrm->fb_size;
	fbi->screen_base = sdrm->fb_map;

	fbi->apertures->ranges[0].base = sdrm->fb_base;
	fbi->apertures->ranges[0].size = sdrm->fb_size;

	return 0;

err_fb_info_destroy:
	drm_fb_helper_release_fbi(helper);

	return ret;
}

static const struct drm_fb_helper_funcs sdrm_fb_helper_funcs = {
	.fb_probe = sdrm_fbdev_create,
};

void sdrm_fbdev_init(struct sdrm_device *sdrm)
{
	struct drm_device *ddev = sdrm->ddev;
	struct drm_fb_helper *fb_helper;
	struct sdrm_fbdev *fbdev;
	int ret;

	fbdev = kzalloc(sizeof(*fbdev), GFP_KERNEL);
	if (!fbdev) {
		dev_err(ddev->dev, "Failed to allocate drm fbdev.\n");
		return;
	}

	fb_helper = &fbdev->fb_helper;

	drm_fb_helper_prepare(ddev, fb_helper, &sdrm_fb_helper_funcs);

	ret = drm_fb_helper_init(ddev, fb_helper, 1, 1);
	if (ret < 0) {
		dev_err(ddev->dev, "Failed to initialize drm fb helper.\n");
		goto err_free;
	}

	ret = drm_fb_helper_single_add_all_connectors(fb_helper);
	if (ret < 0) {
		dev_err(ddev->dev, "Failed to add connectors.\n");
		goto err_drm_fb_helper_fini;
	}

	ret = drm_fb_helper_initial_config(fb_helper,
					   ddev->mode_config.preferred_depth);
	if (ret < 0) {
		dev_err(ddev->dev, "Failed to set initial hw configuration.\n");
		goto err_drm_fb_helper_fini;
	}

	sdrm->fbdev = fbdev;

	return;

err_drm_fb_helper_fini:
	drm_fb_helper_fini(fb_helper);
err_free:
	kfree(fbdev);
}

void sdrm_fbdev_cleanup(struct sdrm_device *sdrm)
{
	struct sdrm_fbdev *fbdev = sdrm->fbdev;
	struct drm_fb_helper *fb_helper;

	if (!fbdev)
		return;

	sdrm->fbdev = NULL;
	fb_helper = &fbdev->fb_helper;

	/* it might have been kicked out */
	if (registered_fb[fbdev->fb_helper.fbdev->node])
		drm_fb_helper_unregister_fbi(fb_helper);

	/* freeing fb_info is done in fb_ops.fb_destroy() */

	drm_framebuffer_unregister_private(&fbdev->fb);
	drm_framebuffer_cleanup(&fbdev->fb);

	drm_fb_helper_fini(fb_helper);
	kfree(fbdev);
}

static void sdrm_fbdev_set_suspend(struct fb_info *fbi, int state)
{
	console_lock();
	fb_set_suspend(fbi, state);
	console_unlock();
}

void netv_fbdev_resume(struct drm_device *drm_dev)
{
	struct sdrm_device *netv = drm_dev->dev_private;
	struct sdrm_fbdev *fbdev = netv->fbdev;

	drm_kms_helper_poll_disable(drm_dev);

/*
	if (fbdev->fb.initialized) {
		console_lock();
		drm_fb_helper_set_suspend(&fbdev->fb.helper, 1);
		console_unlock();
	}
*/
}

void netv_fbdev_suspend(struct drm_device *drm_dev)
{
	struct sdrm_device *netv = drm_dev->dev_private;
	struct sdrm_fbdev *fbdev = netv->fbdev;

	drm_helper_resume_force_mode(drm_dev);

/*
	if (fbdev->fb.initialized) {
		console_lock();
		drm_fb_helper_set_suspend(&fbdev->fb.helper, 0);
		console_unlock();
	}
*/

	drm_kms_helper_poll_enable(drm_dev);
}

/*
 * Since fbdev is using the native framebuffer, fbcon has to be disabled
 * when the drm stack is used.
 */
void sdrm_fbdev_display_pipe_update(struct sdrm_device *sdrm,
				    struct drm_framebuffer *fb)
{
	struct sdrm_fbdev *fbdev = sdrm->fbdev;

	if (!fbdev || fbdev->fb_helper.fb == fb)
		return;

	if (fbdev->fb_helper.fbdev->state == FBINFO_STATE_RUNNING)
		sdrm_fbdev_set_suspend(fbdev->fb_helper.fbdev, 1);
}

void sdrm_fbdev_restore_mode(struct sdrm_device *sdrm)
{
	struct sdrm_fbdev *fbdev = sdrm->fbdev;

	if (!fbdev)
		return;

	drm_fb_helper_restore_fbdev_mode_unlocked(&fbdev->fb_helper);

	if (fbdev->fb_helper.fbdev->state != FBINFO_STATE_RUNNING)
		sdrm_fbdev_set_suspend(fbdev->fb_helper.fbdev, 0);
}

static int sdrm_fbdev_event_notify(struct notifier_block *self,
				   unsigned long action, void *data)
{
	struct sdrm_device *sdrm;
	struct fb_event *event = data;
	struct fb_info *info = event->info;
	struct drm_fb_helper *fb_helper = info->par;

	if (action != FB_EVENT_FB_UNREGISTERED || !fb_helper ||
	    !fb_helper->dev || fb_helper->fbdev != info)
		return NOTIFY_DONE;

	sdrm = fb_helper->dev->dev_private;

	if (sdrm && sdrm->fbdev)
		platform_device_del(sdrm->ddev->platformdev);

	return NOTIFY_DONE;
}

static struct notifier_block sdrm_fbdev_event_notifier = {
	.notifier_call  = sdrm_fbdev_event_notify,
};

void sdrm_fbdev_kickout_init(void)
{
	fb_register_client(&sdrm_fbdev_event_notifier);
}

void sdrm_fbdev_kickout_exit(void)
{
	fb_unregister_client(&sdrm_fbdev_event_notifier);
}
