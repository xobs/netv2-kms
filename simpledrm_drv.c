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
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_data/simplefb.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>

#include "simpledrm.h"

void sdrm_hw_fini(struct drm_device *dev);
int sdrm_hw_init(struct drm_device *dev, uint32_t flags);

static int sdrm_simplefb_load(struct drm_device *ddev, unsigned long flags)
{
	struct sdrm_device *sdrm;
	int ret;

	sdrm = kzalloc(sizeof(*sdrm), GFP_KERNEL);
	if (!sdrm)
		goto err_free;

	ddev->dev_private = sdrm;
	sdrm->ddev = ddev;

	ret = sdrm_hw_init(ddev, flags);
	if (ret)
		goto err_free;

	ret = sdrm_drm_modeset_init(sdrm);
	if (ret)
		goto err_destroy;

	sdrm_fbdev_init(ddev->dev_private);

	DRM_INFO("Initialized %s on minor %d\n", ddev->driver->name,
		 ddev->primary->index);

	return 0;

err_destroy:
	sdrm_hw_fini(ddev);
err_free:
	drm_dev_unref(ddev);
	kfree(sdrm);

	return ret;
}

static int sdrm_simplefb_unload(struct drm_device *ddev)
{
	struct sdrm_device *sdrm = ddev->dev_private;

	sdrm_fbdev_cleanup(sdrm);
	drm_dev_unregister(ddev);
	drm_mode_config_cleanup(ddev);

	/* protect fb_map removal against sdrm_blit() */
	drm_modeset_lock_all(ddev);
	sdrm_hw_fini(ddev);
	drm_modeset_unlock_all(ddev);

	drm_dev_unref(ddev);
	kfree(sdrm);

	return 0;
}

static const struct file_operations sdrm_drm_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.mmap = sdrm_drm_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.unlocked_ioctl = drm_ioctl,
	.release = drm_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.llseek = noop_llseek,
};

static struct drm_driver sdrm_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
			   DRIVER_ATOMIC,
	.fops = &sdrm_drm_fops,
	.lastclose = sdrm_lastclose,

	.gem_free_object = sdrm_gem_free_object,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import = sdrm_gem_prime_import,

	.dumb_create = sdrm_dumb_create,
	.dumb_map_offset = sdrm_dumb_map_offset,
	.dumb_destroy = sdrm_dumb_destroy,

	.name = "simpledrm",
	.desc = "Simple firmware framebuffer DRM driver",
	.date = "20130601",
	.major = 0,
	.minor = 0,
	.patchlevel = 1,

	.load = sdrm_simplefb_load,
	.unload = sdrm_simplefb_unload,
};

/* ---------------------------------------------------------------------- */
/* pm interface                                                           */

#ifdef CONFIG_PM_SLEEP
static int netv_pm_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct drm_device *drm_dev = pci_get_drvdata(pdev);
	struct sdrm_device *netv = drm_dev->dev_private;

/*
	drm_kms_helper_poll_disable(drm_dev);

	if (netv->fb.initialized) {
		console_lock();
		drm_fb_helper_set_suspend(&netv->fb.helper, 1);
		console_unlock();
	}
*/

	return 0;
}

static int netv_pm_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct drm_device *drm_dev = pci_get_drvdata(pdev);
	struct sdrm_device *netv = drm_dev->dev_private;

/*
	drm_helper_resume_force_mode(drm_dev);

	if (netv->fb.initialized) {
		console_lock();
		drm_fb_helper_set_suspend(&netv->fb.helper, 0);
		console_unlock();
	}

	drm_kms_helper_poll_enable(drm_dev);
*/
	return 0;
}
#endif

static const struct dev_pm_ops netv_pm_ops = {
        SET_SYSTEM_SLEEP_PM_OPS(netv_pm_suspend,
                                netv_pm_resume)
};

/* ---------------------------------------------------------------------- */
/* pci interface                                                          */

static int netv_kick_out_firmware_fb(struct pci_dev *pdev)
{
        struct apertures_struct *ap;

        ap = alloc_apertures(1);
        if (!ap)
                return -ENOMEM;

        ap->ranges[0].base = pci_resource_start(pdev, 0);
        ap->ranges[0].size = pci_resource_len(pdev, 0);
        remove_conflicting_framebuffers(ap, "netvdrmfb", false);
        kfree(ap);

        return 0;
}

static int netv_pci_probe(struct pci_dev *pdev,
                           const struct pci_device_id *ent)
{
	int ret;

	ret = netv_kick_out_firmware_fb(pdev);
	if (ret)
		return ret;

	return drm_get_pci_dev(pdev, ent, &sdrm_drm_driver);
}

static void netv_pci_remove(struct pci_dev *pdev)
{
	struct drm_device *dev = pci_get_drvdata(pdev);

	drm_put_dev(dev);
}

static const struct pci_device_id netv_pci_tbl[] = {
	{
		.vendor      = 0x10ee,
		.device      = 0x7021,
		.subvendor   = PCI_ANY_ID, //0x10EE,
		.subdevice   = PCI_ANY_ID, //PCI_ANY_ID,
		.driver_data = 1,
	},
	{ /* end of list */ }
};

static struct pci_driver netv_pci_driver = {
	.name =         "netv-drm",
	.id_table =     netv_pci_tbl,
	.probe =        netv_pci_probe,
	.remove =       netv_pci_remove,
	.driver.pm =    &netv_pm_ops,
};

static int __init sdrm_init(void)
{
	sdrm_fbdev_kickout_init();
	return drm_pci_init(&sdrm_drm_driver, &netv_pci_driver);
}
module_init(sdrm_init);

static void __exit sdrm_exit(void)
{
	sdrm_fbdev_kickout_exit();
	drm_pci_exit(&sdrm_drm_driver, &netv_pci_driver);
}
module_exit(sdrm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Herrmann <dh.herrmann@gmail.com>");
MODULE_DESCRIPTION("Simple firmware framebuffer DRM driver");
MODULE_ALIAS("platform:simple-framebuffer");
