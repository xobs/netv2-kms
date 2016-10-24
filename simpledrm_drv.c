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

#if 0
#if defined CONFIG_OF && defined CONFIG_COMMON_CLK
/*
 * Clock handling code.
 *
 * Here we handle the clocks property of our "simple-framebuffer" dt node.
 * This is necessary so that we can make sure that any clocks needed by
 * the display engine that the bootloader set up for us (and for which it
 * provided a simplefb dt node), stay up, for the life of the simplefb
 * driver.
 *
 * When the driver unloads, we cleanly disable, and then release the clocks.
 *
 * We only complain about errors here, no action is taken as the most likely
 * error can only happen due to a mismatch between the bootloader which set
 * up simplefb, and the clock definitions in the device tree. Chances are
 * that there are no adverse effects, and if there are, a clean teardown of
 * the fb probe will not help us much either. So just complain and carry on,
 * and hope that the user actually gets a working fb at the end of things.
 */
static int sdrm_clocks_init(struct sdrm_device *sdrm,
			    struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct clk *clock;
	int i, ret;

	if (dev_get_platdata(&pdev->dev) || !np)
		return 0;

	sdrm->clk_count = of_clk_get_parent_count(np);
	if (!sdrm->clk_count)
		return 0;

	sdrm->clks = kcalloc(sdrm->clk_count, sizeof(struct clk *), GFP_KERNEL);
	if (!sdrm->clks)
		return -ENOMEM;

	for (i = 0; i < sdrm->clk_count; i++) {
		clock = of_clk_get(np, i);
		if (IS_ERR(clock)) {
			if (PTR_ERR(clock) == -EPROBE_DEFER) {
				while (--i >= 0) {
					if (sdrm->clks[i])
						clk_put(sdrm->clks[i]);
				}
				kfree(sdrm->clks);
				return -EPROBE_DEFER;
			}
			dev_err(&pdev->dev, "%s: clock %d not found: %ld\n",
				__func__, i, PTR_ERR(clock));
			continue;
		}
		sdrm->clks[i] = clock;
	}

	for (i = 0; i < sdrm->clk_count; i++) {
		if (sdrm->clks[i]) {
			ret = clk_prepare_enable(sdrm->clks[i]);
			if (ret) {
				dev_err(&pdev->dev,
					"%s: failed to enable clock %d: %d\n",
					__func__, i, ret);
				clk_put(sdrm->clks[i]);
				sdrm->clks[i] = NULL;
			}
		}
	}

	return 0;
}

static void sdrm_clocks_destroy(struct sdrm_device *sdrm)
{
	int i;

	if (!sdrm->clks)
		return;

	for (i = 0; i < sdrm->clk_count; i++) {
		if (sdrm->clks[i]) {
			clk_disable_unprepare(sdrm->clks[i]);
			clk_put(sdrm->clks[i]);
		}
	}

	kfree(sdrm->clks);
}
#else
static int sdrm_clocks_init(struct sdrm_device *sdrm,
			    struct platform_device *pdev)
{
	return 0;
}

static void sdrm_clocks_destroy(struct sdrm_device *sdrm)
{
}
#endif

#if defined CONFIG_OF && defined CONFIG_REGULATOR

#define SUPPLY_SUFFIX "-supply"

/*
 * Regulator handling code.
 *
 * Here we handle the num-supplies and vin*-supply properties of our
 * "simple-framebuffer" dt node. This is necessary so that we can make sure
 * that any regulators needed by the display hardware that the bootloader
 * set up for us (and for which it provided a simplefb dt node), stay up,
 * for the life of the simplefb driver.
 *
 * When the driver unloads, we cleanly disable, and then release the
 * regulators.
 *
 * We only complain about errors here, no action is taken as the most likely
 * error can only happen due to a mismatch between the bootloader which set
 * up simplefb, and the regulator definitions in the device tree. Chances are
 * that there are no adverse effects, and if there are, a clean teardown of
 * the fb probe will not help us much either. So just complain and carry on,
 * and hope that the user actually gets a working fb at the end of things.
 */
static int sdrm_regulators_init(struct sdrm_device *sdrm,
				struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct regulator *regulator;
	int count = 0, i = 0, ret;
	struct property *prop;
	const char *p;

	if (dev_get_platdata(&pdev->dev) || !np)
		return 0;

	/* Count the number of regulator supplies */
	for_each_property_of_node(np, prop) {
		p = strstr(prop->name, SUPPLY_SUFFIX);
		if (p && p != prop->name)
			count++;
	}

	if (!count)
		return 0;

	sdrm->regulators = devm_kcalloc(&pdev->dev, count,
					sizeof(struct regulator *),
					GFP_KERNEL);
	if (!sdrm->regulators)
		return -ENOMEM;

	/* Get all the regulators */
	for_each_property_of_node(np, prop) {
		char name[32]; /* 32 is max size of property name */

		p = strstr(prop->name, SUPPLY_SUFFIX);
		if (!p || p == prop->name)
			continue;

		strlcpy(name, prop->name,
			strlen(prop->name) - strlen(SUPPLY_SUFFIX) + 1);
		regulator = devm_regulator_get_optional(&pdev->dev, name);
		if (IS_ERR(regulator)) {
			if (PTR_ERR(regulator) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			dev_err(&pdev->dev, "regulator %s not found: %ld\n",
				name, PTR_ERR(regulator));
			continue;
		}
		sdrm->regulators[i++] = regulator;
	}
	sdrm->regulator_count = i;

	/* Enable all the regulators */
	for (i = 0; i < sdrm->regulator_count; i++) {
		ret = regulator_enable(sdrm->regulators[i]);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to enable regulator %d: %d\n",
				i, ret);
			devm_regulator_put(sdrm->regulators[i]);
			sdrm->regulators[i] = NULL;
		}
	}

	return 0;
}

static void sdrm_regulators_destroy(struct sdrm_device *sdrm)
{
	int i;

	if (!sdrm->regulators)
		return;

	for (i = 0; i < sdrm->regulator_count; i++)
		if (sdrm->regulators[i])
			regulator_disable(sdrm->regulators[i]);
}
#else
static int sdrm_regulators_init(struct sdrm_device *sdrm,
				struct platform_device *pdev)
{
	return 0;
}

static void sdrm_regulators_destroy(struct sdrm_device *sdrm)
{
}
#endif

static int parse_dt(struct platform_device *pdev,
		    struct simplefb_platform_data *mode)
{
	struct device_node *np = pdev->dev.of_node;
	const char *format;
	int ret;

	if (!np)
		return -ENODEV;

	ret = of_property_read_u32(np, "width", &mode->width);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse width property\n");
		return ret;
	}

	ret = of_property_read_u32(np, "height", &mode->height);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse height property\n");
		return ret;
	}

	ret = of_property_read_u32(np, "stride", &mode->stride);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse stride property\n");
		return ret;
	}

	ret = of_property_read_string(np, "format", &format);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse format property\n");
		return ret;
	}
	mode->format = format;

	return 0;
}

static struct simplefb_format simplefb_formats[] = SIMPLEFB_FORMATS;

int sdrm_pdev_init(struct sdrm_device *sdrm)
{
	struct platform_device *pdev = sdrm->ddev->platformdev;
	struct simplefb_platform_data *mode = pdev->dev.platform_data;
	struct simplefb_platform_data pmode;
	struct resource *mem;
	unsigned int depth;
	int ret, i, bpp;

	if (!mode) {
		mode = &pmode;
		ret = parse_dt(pdev, mode);
		if (ret)
			return ret;
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(sdrm->ddev->dev, "No memory resource\n");
		return -ENODEV;
	}

	for (i = 0; i < ARRAY_SIZE(simplefb_formats); ++i) {
		if (strcmp(mode->format, simplefb_formats[i].name))
			continue;

		sdrm->fb_sformat = &simplefb_formats[i];
		sdrm->fb_format = simplefb_formats[i].fourcc;
		sdrm->fb_width = mode->width;
		sdrm->fb_height = mode->height;
		sdrm->fb_stride = mode->stride;
		sdrm->fb_base = mem->start;
		sdrm->fb_size = resource_size(mem);
		break;
	}

	if (i >= ARRAY_SIZE(simplefb_formats)) {
		dev_err(sdrm->ddev->dev, "Unknown format %s\n", mode->format);
		return -ENODEV;
	}

	switch (sdrm->fb_format) {
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		/*
		 * You must adjust sdrm_put() whenever you add a new format
		 * here, otherwise, blitting operations will not work.
		 * Furthermore, include/linux/platform_data/simplefb.h needs
		 * to be adjusted so the platform-device actually allows this
		 * format.
		 */
		break;
	default:
		dev_err(sdrm->ddev->dev, "Unsupported format %s\n",
			mode->format);
		return -ENODEV;
	}

	drm_fb_get_bpp_depth(sdrm->fb_format, &depth, &bpp);
	if (!bpp) {
		dev_err(sdrm->ddev->dev, "Unknown format %s\n", mode->format);
		return -ENODEV;
	}

	if (sdrm->fb_size < sdrm->fb_stride * sdrm->fb_height) {
		dev_err(sdrm->ddev->dev, "FB too small\n");
		return -ENODEV;
	} else if ((bpp + 7) / 8 * sdrm->fb_width > sdrm->fb_stride) {
		dev_err(sdrm->ddev->dev, "Invalid stride\n");
		return -ENODEV;
	}

	sdrm->fb_bpp = bpp;

	sdrm->fb_map = ioremap_wc(sdrm->fb_base, sdrm->fb_size);
	if (!sdrm->fb_map) {
		dev_err(sdrm->ddev->dev, "cannot remap VMEM\n");
		return -EIO;
	}

	DRM_DEBUG_KMS("format: %s\n", drm_get_format_name(sdrm->fb_format));

	return 0;
}

void sdrm_pdev_destroy(struct sdrm_device *sdrm)
{
	if (sdrm->fb_map) {
		iounmap(sdrm->fb_map);
		sdrm->fb_map = NULL;
	}
}
#endif

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
//	ret = sdrm_pdev_init(sdrm);
	if (ret)
		goto err_free;

	ret = sdrm_drm_modeset_init(sdrm);
	if (ret)
		goto err_destroy;

//	ret = sdrm_clocks_init(sdrm, pdev);
//	if (ret < 0)
//		goto err_cleanup;
//
//	ret = sdrm_regulators_init(sdrm, pdev);
//	if (ret < 0)
//		goto err_clocks;

	//platform_set_drvdata(pdev, ddev);
	//ret = drm_dev_register(ddev, 0);
	//if (ret)
	//	goto err_regulators;

	sdrm_fbdev_init(ddev->dev_private);

	DRM_INFO("Initialized %s on minor %d\n", ddev->driver->name,
		 ddev->primary->index);

	return 0;

//err_regulators:
//	sdrm_regulators_destroy(sdrm);
//err_clocks:
//	sdrm_clocks_destroy(sdrm);
//err_cleanup:
//	drm_mode_config_cleanup(ddev);
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

//	sdrm_regulators_destroy(sdrm);
//	sdrm_clocks_destroy(sdrm);

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
