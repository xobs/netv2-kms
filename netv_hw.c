/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drm_vma_manager.h>

#include "simpledrm.h"

#include <linux/io.h>
#include <linux/fb.h>
#include <linux/console.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>

/*
 * Data-Format for Simple-Framebuffers
 * @name: unique 0-terminated name that can be used to identify the mode
 * @red,green,blue: Offsets and sizes of the single RGB parts
 * @transp: Offset and size of the alpha bits. length=0 means no alpha
 * @fourcc: 32bit DRM four-CC code (see drm_fourcc.h)
 */
struct simplefb_format {
        const char *name;
        u32 bits_per_pixel;
        struct fb_bitfield red;
        struct fb_bitfield green;
        struct fb_bitfield blue;
        struct fb_bitfield transp;
        u32 fourcc;
};

//        { "r5g6b5", 16, {11, 5}, {5, 6}, {0, 5}, {0, 0}, DRM_FORMAT_RGB565 },
//        { "x1r5g5b5", 16, {10, 5}, {5, 5}, {0, 5}, {0, 0}, DRM_FORMAT_XRGB1555 },
//        { "a1r5g5b5", 16, {10, 5}, {5, 5}, {0, 5}, {15, 1}, DRM_FORMAT_ARGB1555 },
//        { "x2r10g10b10", 32, {20, 10}, {10, 10}, {0, 10}, {0, 0}, DRM_FORMAT_XRGB2101010 },
//        { "a2r10g10b10", 32, {20, 10}, {10, 10}, {0, 10}, {30, 2}, DRM_FORMAT_ARGB2101010 },

#define SIMPLEFB_FORMATS \
{ \
        { "a8b8g8r8", 32, {0, 8}, {8, 8}, {16, 8}, {24, 8}, DRM_FORMAT_ABGR8888 }, \
        { "x8r8g8b8", 32, {16, 8}, {8, 8}, {0, 8}, {0, 0}, DRM_FORMAT_XRGB8888 }, \
        { "r8g8b8", 24, {16, 8}, {8, 8}, {0, 8}, {0, 0}, DRM_FORMAT_RGB888 }, \
        { "a8r8g8b8", 32, {16, 8}, {8, 8}, {0, 8}, {24, 8}, DRM_FORMAT_ARGB8888 }, \
}

static struct simplefb_format simplefb_formats[] = SIMPLEFB_FORMATS;

/* ---------------------------------------------------------------------- */

#if 0
static void netv_vga_writeb(struct sdrm_device *netv, u16 ioport, u8 val)
{
#if 0
	if (WARN_ON(ioport < 0x3c0 || ioport > 0x3df))
		return;

	if (netv->mmio) {
		int offset = ioport - 0x3c0 + 0x400;
		writeb(val, netv->mmio + offset);
	} else {
		outb(val, ioport);
	}
#endif
}

static u16 netv_dispi_read(struct sdrm_device *netv, u16 reg)
{
	u16 ret = 0;
#if 0
	if (netv->mmio) {
		int offset = 0x500 + (reg << 1);
		ret = readw(netv->mmio + offset);
	} else {
		outw(reg, VBE_DISPI_IOPORT_INDEX);
		ret = inw(VBE_DISPI_IOPORT_DATA);
	}
#endif
	return ret;
}

static void netv_dispi_write(struct sdrm_device *netv, u16 reg, u16 val)
{
#if 0
	if (netv->mmio) {
		int offset = 0x500 + (reg << 1);
		writew(val, netv->mmio + offset);
	} else {
		outw(reg, VBE_DISPI_IOPORT_INDEX);
		outw(val, VBE_DISPI_IOPORT_DATA);
	}
#endif
}
#else
#define netv_vga_writeb(netv, ioport, val)
#define netv_dispi_read(netv, reg)
#define netv_dispi_write(netv, reg, val)
#endif

int sdrm_hw_init(struct drm_device *dev, uint32_t flags)
{
	struct sdrm_device *netv = dev->dev_private;
	struct pci_dev *pdev = dev->pdev;
	unsigned long addr, size, mem, ioaddr, iosize;
	u16 id;

#if 0
	if (pdev->resource[0].flags & IORESOURCE_MEM) {
		/* mmio bar with vga and netv registers present */
		if (pci_request_region(pdev, 0, "netv-drm") != 0) {
			DRM_ERROR("Cannot request mmio region\n");
			return -EBUSY;
		}
		ioaddr = pci_resource_start(pdev, 0);
		iosize = pci_resource_len(pdev, 0);
		netv->mmio = ioremap(ioaddr, iosize);
		if (netv->mmio == NULL) {
			DRM_ERROR("Cannot map mmio region\n");
			return -ENOMEM;
		}
	} else {
		ioaddr = VBE_DISPI_IOPORT_INDEX;
		iosize = 2;
		if (!request_region(ioaddr, iosize, "netv-drm")) {
			DRM_ERROR("Cannot request ioports\n");
			return -EBUSY;
		}
		netv->ioports = 1;
	}

		id = netv_dispi_read(netv, VBE_DISPI_INDEX_ID);
	mem = netv_dispi_read(netv, VBE_DISPI_INDEX_VIDEO_MEMORY_64K)
		* 64 * 1024;
	
		if ((id & 0xfff0) != VBE_DISPI_ID0) {
			DRM_ERROR("ID mismatch\n");
			return -ENODEV;
		}
#else
	ioaddr = 0;
	iosize = 2;
	id = 0x1254;
#endif

	if ((pdev->resource[0].flags & IORESOURCE_MEM) == 0)
		return -ENODEV;
	addr = pci_resource_start(pdev, 0);
	mem = size = pci_resource_len(pdev, 0);
	if (addr == 0)
		return -ENODEV;
	if (size != mem) {
		DRM_ERROR("Size mismatch: pci=%ld, netv=%ld\n",
			size, mem);
		DRM_ERROR("Making \"mem\" equal to \"size\"\n");
		size = min(size, mem);
	}

	if (pci_request_region(pdev, 0, "netv-drm") != 0) {
		DRM_ERROR("Cannot request framebuffer\n");
		return -EBUSY;
	}


/*
	netv->fb_sformat = &simplefb_formats[i];
	netv->fb_format = simplefb_formats[i].fourcc;
	netv->fb_width = mode->width;
	netv->fb_height = mode->height;
	netv->fb_stride = mode->stride;
*/

	netv->fb_base = addr;
	netv->fb_size = size;

	netv->fb_map = ioremap_wc(addr, size);
	if (netv->fb_map == NULL) {
		DRM_ERROR("Cannot map framebuffer\n");
		return -ENOMEM;
	}

	netv->fb_sformat = &simplefb_formats[0];
	netv->fb_format = simplefb_formats[0].fourcc;
	netv->fb_bpp = simplefb_formats[0].bits_per_pixel;
	netv->fb_width = 1920;
	netv->fb_height = 1080;
	netv->fb_stride = netv->fb_width * 3;//(netv->fb_bpp / 8);

	DRM_INFO("Found NeTV device, ID 0x%x.\n", id);
	DRM_INFO("Framebuffer size %ld kB @ 0x%lx, @ 0x%lx.\n",
		 size / 1024, addr,
		 ioaddr);
	DRM_INFO("%dx%d @ %d bpp\n", netv->fb_width, netv->fb_height, netv->fb_bpp);

	/*
	if (netv->mmio && pdev->revision >= 2) {
		qext_size = readl(netv->mmio + 0x600);
		if (qext_size < 4 || qext_size > iosize)
			goto noext;
		DRM_DEBUG("Found qemu ext regs, size %ld\n", qext_size);
		if (qext_size >= 8) {
#ifdef __BIG_ENDIAN
			writel(0xbebebebe, netv->mmio + 0x604);
#else
			writel(0x1e1e1e1e, netv->mmio + 0x604);
#endif
			DRM_DEBUG("  qext endian: 0x%x\n",
				  readl(netv->mmio + 0x604));
		}
	}
	*/

noext:
	return 0;
}

void sdrm_hw_fini(struct drm_device *dev)
{
	struct sdrm_device *netv = dev->dev_private;

//	if (netv->mmio)
//		iounmap(netv->mmio);
//	if (netv->ioports)
//		release_region(VBE_DISPI_IOPORT_INDEX, 2);
	if (netv->fb_map)
		iounmap(netv->fb_map);
	pci_release_region(dev->pdev, 0);
}

void netv_hw_setmode(struct sdrm_device *netv,
		      struct drm_display_mode *mode)
{
#if 0
	netv->xres = mode->hdisplay;
	netv->yres = mode->vdisplay;
	netv->bpp = 24;
	netv->stride = mode->hdisplay * (netv->bpp / 8);
	netv->yres_virtual = netv->fb_size / netv->stride;

	DRM_DEBUG_DRIVER("%dx%d @ %d bpp, vy %d\n",
			 netv->xres, netv->yres, netv->bpp,
			 netv->yres_virtual);

	netv_vga_writeb(netv, 0x3c0, 0x20); /* unblank */

	netv_dispi_write(netv, VBE_DISPI_INDEX_ENABLE,      0);
	netv_dispi_write(netv, VBE_DISPI_INDEX_BPP,         netv->bpp);
	netv_dispi_write(netv, VBE_DISPI_INDEX_XRES,        netv->xres);
	netv_dispi_write(netv, VBE_DISPI_INDEX_YRES,        netv->yres);
	netv_dispi_write(netv, VBE_DISPI_INDEX_BANK,        0);
	netv_dispi_write(netv, VBE_DISPI_INDEX_VIRT_WIDTH,  netv->xres);
	netv_dispi_write(netv, VBE_DISPI_INDEX_VIRT_HEIGHT,
			  netv->yres_virtual);
	netv_dispi_write(netv, VBE_DISPI_INDEX_X_OFFSET,    0);
	netv_dispi_write(netv, VBE_DISPI_INDEX_Y_OFFSET,    0);

	netv_dispi_write(netv, VBE_DISPI_INDEX_ENABLE,
			  VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
#endif
}

void netv_hw_setbase(struct sdrm_device *netv,
		      int x, int y, u64 addr)
{
#if 0
	unsigned long offset = (unsigned long)addr +
		y * netv->stride +
		x * (netv->bpp / 8);
	int vy = offset / netv->stride;
	int vx = (offset % netv->stride) * 8 / netv->bpp;

	DRM_DEBUG_DRIVER("x %d, y %d, addr %llx -> offset %lx, vx %d, vy %d\n",
			 x, y, addr, offset, vx, vy);
	netv_dispi_write(netv, VBE_DISPI_INDEX_X_OFFSET, vx);
	netv_dispi_write(netv, VBE_DISPI_INDEX_Y_OFFSET, vy);
#endif
}
