/*
 * Copyright 2007 Nouveau Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include <sys/mman.h>
#include <sys/ioctl.h>

#include "nouveau_drmif.h"
#include "nouveau_local.h"

int
nouveau_bo_init(struct nouveau_device *dev)
{
	return 0;
}

void
nouveau_bo_takedown(struct nouveau_device *dev)
{
}

static int
nouveau_bo_allocated(struct nouveau_bo_priv *nvbo)
{
	if (nvbo->sysmem || nvbo->handle)
		return 1;
	return 0;
}

static int
nouveau_bo_ualloc(struct nouveau_bo_priv *nvbo)
{
	if (nvbo->user || nvbo->sysmem) {
		assert(nvbo->sysmem);
		return 0;
	}

	nvbo->sysmem = malloc(nvbo->size);
	if (!nvbo->sysmem)
		return -ENOMEM;

	return 0;
}

static void
nouveau_bo_ufree(struct nouveau_bo_priv *nvbo)
{
	if (nvbo->sysmem) {
		if (!nvbo->user)
			free(nvbo->sysmem);
		nvbo->sysmem = NULL;
	}
}

static void
nouveau_bo_kfree(struct nouveau_bo_priv *nvbo)
{
	struct nouveau_device_priv *nvdev = nouveau_device(nvbo->base.device);
	struct drm_gem_close req;

	if (!nvbo->handle)
		return;

	if (nvbo->map) {
		munmap(nvbo->map, nvbo->size);
		nvbo->map = NULL;
	}

	req.handle = nvbo->handle;
	nvbo->handle = 0;
	ioctl(nvdev->fd, DRM_IOCTL_GEM_CLOSE, &req);
}

static int
nouveau_bo_kalloc(struct nouveau_bo_priv *nvbo)
{
	struct nouveau_device_priv *nvdev = nouveau_device(nvbo->base.device);
	struct drm_nouveau_gem_new req;
	int ret;

	if (nvbo->handle)
		return 0;

	req.size = nvbo->size;
	req.align = nvbo->align;

	req.domain = 0;

	if (nvbo->flags & NOUVEAU_BO_VRAM)
		req.domain |= NOUVEAU_GEM_DOMAIN_VRAM;

	if (nvbo->flags & NOUVEAU_BO_GART)
		req.domain |= NOUVEAU_GEM_DOMAIN_GART;

	if (nvbo->flags & NOUVEAU_BO_TILED) {
		req.domain |= NOUVEAU_GEM_DOMAIN_TILE;
		if (nvbo->flags & NOUVEAU_BO_ZTILE)
			req.domain |= NOUVEAU_GEM_DOMAIN_TILE_ZETA;
	}

	if (!req.domain) {
		req.domain |= (NOUVEAU_GEM_DOMAIN_VRAM |
			       NOUVEAU_GEM_DOMAIN_GART);
	}

	ret = drmCommandWriteRead(nvdev->fd, DRM_NOUVEAU_GEM_NEW,
				  &req, sizeof(req));
	if (ret)
		return ret;
	nvbo->handle = req.handle;
	nvbo->size = req.size;
	nvbo->domain = req.domain;

	return 0;
}

static int
nouveau_bo_kmap(struct nouveau_bo_priv *nvbo)
{
	struct nouveau_device_priv *nvdev = nouveau_device(nvbo->base.device);
	struct drm_nouveau_gem_mmap req;
	int ret;

	if (nvbo->map)
		return 0;

	if (!nvbo->handle)
		return -EINVAL;

	req.handle = nvbo->handle;
	ret = drmCommandWriteRead(nvdev->fd, DRM_NOUVEAU_GEM_MMAP,
				  &req, sizeof(req));
	if (ret)
		return ret;

	nvbo->map = (void *)(unsigned long)req.vaddr;
	return 0;
}

int
nouveau_bo_new(struct nouveau_device *dev, uint32_t flags, int align,
	       int size, struct nouveau_bo **bo)
{
	struct nouveau_bo_priv *nvbo;
	int ret;

	if (!dev || !bo || *bo)
		return -EINVAL;

	nvbo = calloc(1, sizeof(struct nouveau_bo_priv));
	if (!nvbo)
		return -ENOMEM;
	nvbo->base.device = dev;
	nvbo->base.size = size;
	nvbo->base.handle = bo_to_ptr(nvbo);

	nvbo->refcount = 1;
	nvbo->flags = flags;
	nvbo->size = size;
	nvbo->align = align;

	/*XXX: murder me violently */
	if (flags & NOUVEAU_BO_TILED) {
		nvbo->base.tiled = 1;
		if (flags & NOUVEAU_BO_ZTILE)
			nvbo->base.tiled |= 2;
	}

	if (flags & NOUVEAU_BO_PIN) {
		ret = nouveau_bo_pin((void *)nvbo, nvbo->flags);
		if (ret) {
			nouveau_bo_ref(NULL, (void *)nvbo);
			return ret;
		}
	}

	*bo = &nvbo->base;
	return 0;
}

int
nouveau_bo_user(struct nouveau_device *dev, void *ptr, int size,
		struct nouveau_bo **bo)
{
	struct nouveau_bo_priv *nvbo;
	int ret;

	ret = nouveau_bo_new(dev, 0, 0, size, bo);
	if (ret)
		return ret;
	nvbo = nouveau_bo(*bo);

	nvbo->sysmem = ptr;
	nvbo->user = 1;
	return 0;
}

int
nouveau_bo_handle_get(struct nouveau_bo *bo, uint32_t *handle)
{
	struct nouveau_bo_priv *nvbo = nouveau_bo(bo);
	int ret;
 
	if (!bo || !handle)
		return -EINVAL;
 
	if (!nvbo->global_handle) {
		struct nouveau_device_priv *nvdev = nouveau_device(bo->device);
		struct drm_gem_flink req;
 
		ret = nouveau_bo_kalloc(nvbo);
		if (ret)
			return ret;
 
		req.handle = nvbo->handle;
		ret = ioctl(nvdev->fd, DRM_IOCTL_GEM_FLINK, &req);
		if (ret) {
			nouveau_bo_kfree(nvbo);
			return ret;
		}
 
		nvbo->global_handle = req.name;
	}
 
	*handle = nvbo->global_handle;
	return 0;
}
 
int
nouveau_bo_handle_ref(struct nouveau_device *dev, uint32_t handle,
		      struct nouveau_bo **bo)
{
	struct nouveau_device_priv *nvdev = nouveau_device(dev);
	struct nouveau_bo_priv *nvbo;
	struct drm_gem_open req;
	int ret;
 
	ret = nouveau_bo_new(dev, 0, 0, 0, bo);
	if (ret)
		return ret;
	nvbo = nouveau_bo(*bo);
 
	req.name = handle;
	ret = ioctl(nvdev->fd, DRM_IOCTL_GEM_OPEN, &req);
	if (ret) {
		nouveau_bo_ref(NULL, bo);
		return ret;
	}
 
	nvbo->size = req.size;
	nvbo->handle = req.handle;
	return 0;
} 

static void
nouveau_bo_del(struct nouveau_bo **bo)
{
	struct nouveau_bo_priv *nvbo;

	if (!bo || !*bo)
		return;
	nvbo = nouveau_bo(*bo);
	*bo = NULL;

	if (--nvbo->refcount)
		return;

	if (nvbo->pending)
		nouveau_pushbuf_flush(nvbo->pending_channel, 0);

	nouveau_fence_ref(NULL, &nvbo->fence);
	nouveau_bo_ufree(nvbo);
	nouveau_bo_kfree(nvbo);
	free(nvbo);
}

int
nouveau_bo_ref(struct nouveau_bo *ref, struct nouveau_bo **pbo)
{
	if (!pbo)
		return -EINVAL;

	if (ref)
		nouveau_bo(ref)->refcount++;

	if (*pbo)
		nouveau_bo_del(pbo);

	*pbo = ref;
	return 0;
}

int
nouveau_bo_map(struct nouveau_bo *bo, uint32_t flags)
{
	struct nouveau_bo_priv *nvbo = nouveau_bo(bo);
	int ret;

	if (!nvbo || bo->map)
		return -EINVAL;

	if (!nouveau_bo_allocated(nvbo)) {
		if (nvbo->flags & (NOUVEAU_BO_VRAM | NOUVEAU_BO_GART))
			nouveau_bo_kalloc(nvbo);

		if (!nouveau_bo_allocated(nvbo)) {
			ret = nouveau_bo_ualloc(nvbo);
			if (ret)
				return ret;
		}
	}

	if (nvbo->sysmem) {
		bo->map = nvbo->sysmem;
	} else {
		if (nvbo->pending &&
		    (nvbo->pending->write_domains || flags & NOUVEAU_BO_WR)) {
			nouveau_pushbuf_flush(nvbo->pending_channel, 0);
		}

		nouveau_bo_kmap(nvbo);

		if (flags & NOUVEAU_BO_WR)
			nouveau_fence_wait(&nvbo->fence);
		else
			nouveau_fence_wait(&nvbo->wr_fence);

		bo->map = nvbo->map;
	}

	return 0;
}

void
nouveau_bo_unmap(struct nouveau_bo *bo)
{
	bo->map = NULL;
}

uint64_t
nouveau_bo_get_drm_map(struct nouveau_bo *bo)
{
	NOUVEAU_ERR("-EINVAL :)\n");
	return 0;
}

int
nouveau_bo_pin(struct nouveau_bo *bo, uint32_t flags)
{
	struct nouveau_device_priv *nvdev = nouveau_device(bo->device);
	struct nouveau_bo_priv *nvbo = nouveau_bo(bo);
	struct drm_nouveau_gem_pin req;
	int ret;

	if (nvbo->pinned)
		return 0;

	/* Ensure we have a kernel object... */
	if (!nvbo->handle) {
		if (!(flags & (NOUVEAU_BO_VRAM | NOUVEAU_BO_GART)))
			return -EINVAL;
		nvbo->flags = flags;

		ret = nouveau_bo_kalloc(nvbo);
		if (ret)
			return ret;
	}

	/* Now force it to stay put :) */
	req.handle = nvbo->handle;
	req.domain = 0;
	if (nvbo->flags & NOUVEAU_BO_VRAM)
		req.domain |= NOUVEAU_GEM_DOMAIN_VRAM;
	if (nvbo->flags & NOUVEAU_BO_GART)
		req.domain |= NOUVEAU_GEM_DOMAIN_GART;

	ret = drmCommandWriteRead(nvdev->fd, DRM_NOUVEAU_GEM_PIN, &req,
				  sizeof(struct drm_nouveau_gem_pin));
	if (ret)
		return ret;
	nvbo->offset = req.offset;
	nvbo->domain = req.domain;
	nvbo->pinned = 1;

	/* Fill in public nouveau_bo members */
	if (nvbo->domain & NOUVEAU_GEM_DOMAIN_VRAM)
		bo->flags = NOUVEAU_BO_VRAM;
	if (nvbo->domain & NOUVEAU_GEM_DOMAIN_GART)
		bo->flags = NOUVEAU_BO_GART;
	bo->offset = nvbo->offset;

	return 0;
}

void
nouveau_bo_unpin(struct nouveau_bo *bo)
{
	struct nouveau_device_priv *nvdev = nouveau_device(bo->device);
	struct nouveau_bo_priv *nvbo = nouveau_bo(bo);
	struct drm_nouveau_gem_unpin req;

	if (!nvbo->pinned)
		return;

	req.handle = nvbo->handle;
	drmCommandWrite(nvdev->fd, DRM_NOUVEAU_GEM_UNPIN, &req, sizeof(req));

	nvbo->pinned = bo->offset = bo->flags = 0;
}

struct drm_nouveau_gem_pushbuf_bo *
nouveau_bo_emit_buffer(struct nouveau_channel *chan, struct nouveau_bo *bo)
{
	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(chan->pushbuf);
	struct nouveau_bo_priv *nvbo = nouveau_bo(bo);
	struct drm_nouveau_gem_pushbuf_bo *pbbo;
	struct nouveau_bo *ref = NULL;
	int ret;

	if (nvbo->pending)
		return nvbo->pending;

	if (!nvbo->handle) {
		nouveau_bo_kalloc(nvbo);
		if (nvbo->sysmem) {
			void *sysmem_tmp = nvbo->sysmem;

			nvbo->sysmem = NULL;
			ret = nouveau_bo_map(bo, NOUVEAU_BO_WR);
			if (ret)
				return NULL;
			nvbo->sysmem = sysmem_tmp;

			memcpy(bo->map, nvbo->sysmem, nvbo->size);
			nouveau_bo_unmap(bo);
			nouveau_bo_ufree(nvbo);
		}
	}

	if (nvpb->nr_buffers >= NOUVEAU_PUSHBUF_MAX_BUFFERS)
		return NULL;
	pbbo = nvpb->buffers + nvpb->nr_buffers++;
	nvbo->pending = pbbo;
	nvbo->pending_channel = chan;

	nouveau_bo_ref(bo, &ref);
	pbbo->user_priv = (uint64_t)(unsigned long)ref;
	pbbo->handle = nvbo->handle;
	pbbo->valid_domains = NOUVEAU_GEM_DOMAIN_VRAM | NOUVEAU_GEM_DOMAIN_GART;
	pbbo->read_domains =
	pbbo->write_domains = 0;
	pbbo->presumed_domain = nvbo->domain;
	pbbo->presumed_offset = nvbo->offset;
	pbbo->presumed_ok = 1;
	return pbbo;
}
