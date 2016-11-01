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
#include <linux/dma-buf.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "simpledrm.h"


int sdrm_gem_get_pages(struct sdrm_gem_object *obj)
{
	size_t num, i;

	if (obj->vmapping)
		return 0;

	if (obj->base.import_attach) {
		obj->vmapping = dma_buf_vmap(obj->base.import_attach->dmabuf);
		return !obj->vmapping ? -ENOMEM : 0;
	}

	num = obj->base.size >> PAGE_SHIFT;
	obj->pages = drm_malloc_ab(num, sizeof(*obj->pages));
	if (!obj->pages)
		return -ENOMEM;

	for (i = 0; i < num; ++i) {
		obj->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!obj->pages[i])
			goto error;
	}

	obj->vmapping = vmap(obj->pages, num, 0, PAGE_KERNEL);
	if (!obj->vmapping)
		goto error;

	return 0;

error:
	while (i > 0)
		__free_pages(obj->pages[--i], 0);

	drm_free_large(obj->pages);
	obj->pages = NULL;
	return -ENOMEM;
}

static void sdrm_gem_put_pages(struct sdrm_gem_object *obj)
{
	size_t num, i;

	if (!obj->vmapping)
		return;

	if (obj->base.import_attach) {
		dma_buf_vunmap(obj->base.import_attach->dmabuf, obj->vmapping);
		obj->vmapping = NULL;
		return;
	}

	vunmap(obj->vmapping);
	obj->vmapping = NULL;

	num = obj->base.size >> PAGE_SHIFT;
	for (i = 0; i < num; ++i)
		__free_pages(obj->pages[i], 0);

	drm_free_large(obj->pages);
	obj->pages = NULL;
}

struct sdrm_gem_object *sdrm_gem_alloc_object(struct drm_device *ddev,
					      size_t size)
{
	struct sdrm_gem_object *obj;

	WARN_ON(!size || (size & ~PAGE_MASK) != 0);

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return NULL;

	drm_gem_private_object_init(ddev, &obj->base, size);
	return obj;
}

void sdrm_gem_free_object(struct drm_gem_object *gobj)
{
	struct sdrm_gem_object *obj = to_sdrm_bo(gobj);
	struct drm_device *ddev = gobj->dev;

	if (obj->pages) {
		/* kill all user-space mappings */
		drm_vma_node_unmap(&gobj->vma_node,
				   ddev->anon_inode->i_mapping);
	}
	sdrm_gem_put_pages(obj);

	if (gobj->import_attach)
		drm_prime_gem_destroy(gobj, obj->sg);

	drm_gem_free_mmap_offset(gobj);
	drm_gem_object_release(gobj);
	kfree(obj);
}

int sdrm_dumb_create(struct drm_file *dfile, struct drm_device *ddev,
		     struct drm_mode_create_dumb *args)
{
	struct sdrm_gem_object *obj;
	int r;

	if (args->flags)
		return -EINVAL;

	/* overflow checks are done by DRM core */
	args->pitch = (args->bpp + 7) / 8 * args->width;
	args->size = PAGE_ALIGN(args->pitch * args->height);

	obj = sdrm_gem_alloc_object(ddev, args->size);
	if (!obj)
		return -ENOMEM;

	r = drm_gem_handle_create(dfile, &obj->base, &args->handle);
	if (r) {
		drm_gem_object_unreference_unlocked(&obj->base);
		return r;
	}

	/* handle owns a reference */
	drm_gem_object_unreference_unlocked(&obj->base);
	return 0;
}

int sdrm_dumb_destroy(struct drm_file *dfile, struct drm_device *ddev,
		      uint32_t handle)
{
	return drm_gem_handle_delete(dfile, handle);
}

int sdrm_dumb_map_offset(struct drm_file *dfile, struct drm_device *ddev,
			 uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *gobj;
	int r;

	mutex_lock(&ddev->struct_mutex);

	gobj = drm_gem_object_lookup(dfile, handle);
	if (!gobj) {
		r = -ENOENT;
		goto out_unlock;
	}

	r = drm_gem_create_mmap_offset(gobj);
	if (r)
		goto out_unref;

	*offset = drm_vma_node_offset_addr(&gobj->vma_node);

out_unref:
	drm_gem_object_unreference(gobj);
out_unlock:
	mutex_unlock(&ddev->struct_mutex);
	return r;
}

static void sdma_vm_close(struct vm_area_struct *vma)
{
	struct sdrm_gem_object *obj;

	obj = vma->vm_private_data;
	sdrm_gem_put_pages(obj);

	vma->vm_private_data = NULL;
}

static const struct vm_operations_struct sdrm_gem_vm_ops = {
	.close = sdma_vm_close,
};

int sdrm_drm_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *priv = filp->private_data;
	struct drm_device *dev = priv->minor->dev;
	struct drm_vma_offset_node *node;
	struct drm_gem_object *gobj;
	struct sdrm_gem_object *obj;
	size_t size, i, num;
	int r;

	if (drm_device_is_unplugged(dev))
		return -ENODEV;

	drm_vma_offset_lock_lookup(dev->vma_offset_manager);
	node = drm_vma_offset_exact_lookup_locked(dev->vma_offset_manager,
						  vma->vm_pgoff,
						  vma_pages(vma));
	drm_vma_offset_unlock_lookup(dev->vma_offset_manager);

	if (!drm_vma_node_is_allowed(node, filp))
		return -EACCES;

	gobj = container_of(node, struct drm_gem_object, vma_node);
	obj = to_sdrm_bo(gobj);
	size = drm_vma_node_size(node) << PAGE_SHIFT;
	if (size < vma->vm_end - vma->vm_start)
		return r;

	r = sdrm_gem_get_pages(obj);
	if (r < 0)
		return r;

	/* prevent dmabuf-imported mmap to user-space */
	if (!obj->pages)
		return -EACCES;

	vma->vm_flags |= VM_DONTEXPAND;
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));

	vma->vm_ops = &sdrm_gem_vm_ops;
	vma->vm_private_data = obj;

	num = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	for (i = 0; i < num; ++i) {
		r = vm_insert_page(vma, vma->vm_start + i * PAGE_SIZE,
				   obj->pages[i]);
		if (r < 0) {
			if (i > 0)
				zap_vma_ptes(vma, vma->vm_start, i * PAGE_SIZE);
			return r;
		}
	}

	return 0;
}

struct drm_gem_object *sdrm_gem_prime_import(struct drm_device *ddev,
					     struct dma_buf *dma_buf)
{
	struct dma_buf_attachment *attach;
	struct sdrm_gem_object *obj;
	struct sg_table *sg;
	int ret;

	/* need to attach */
	attach = dma_buf_attach(dma_buf, ddev->dev);
	if (IS_ERR(attach))
		return ERR_CAST(attach);

	get_dma_buf(dma_buf);

	sg = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sg)) {
		ret = PTR_ERR(sg);
		goto fail_detach;
	}

	/*
	 * dma_buf_vmap() gives us a page-aligned mapping, so lets bump the
	 * size of the dma-buf to the next page-boundary
	 */
	obj = sdrm_gem_alloc_object(ddev, PAGE_ALIGN(dma_buf->size));
	if (!obj) {
		ret = -ENOMEM;
		goto fail_unmap;
	}

	obj->sg = sg;
	obj->base.import_attach = attach;

	return &obj->base;

fail_unmap:
	dma_buf_unmap_attachment(attach, sg, DMA_BIDIRECTIONAL);
fail_detach:
	dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);
	return ERR_PTR(ret);
}
