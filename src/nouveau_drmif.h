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

#ifndef __NOUVEAU_DRMIF_H__
#define __NOUVEAU_DRMIF_H__

#include <stdint.h>
#include <xf86drm.h>
#include <nouveau_drm.h>

#include "nouveau_device.h"
#include "nouveau_channel.h"
#include "nouveau_grobj.h"
#include "nouveau_notifier.h"
#include "nouveau_bo.h"
#include "nouveau_resource.h"
#include "nouveau_pushbuf.h"
#include "nouveau_local.h"

struct nouveau_device_priv {
	struct nouveau_device base;

	int fd;
	drm_context_t ctx;
	drmLock *lock;
	int needs_close;
};
#define nouveau_device(n) ((struct nouveau_device_priv *)(n))

NOUVEAU_PRIVATE int
nouveau_device_open_existing(struct nouveau_device **, int close,
			     int fd, drm_context_t ctx);

NOUVEAU_PRIVATE int
nouveau_device_open(struct nouveau_device **, const char *busid);

NOUVEAU_PRIVATE void
nouveau_device_close(struct nouveau_device **);

NOUVEAU_PRIVATE int
nouveau_device_get_param(struct nouveau_device *, uint64_t param, uint64_t *v);

NOUVEAU_PRIVATE int
nouveau_device_set_param(struct nouveau_device *, uint64_t param, uint64_t val);

struct nouveau_fence {
	struct nouveau_channel *channel;
};

struct nouveau_fence_priv {
	struct nouveau_fence base;
	int refcount;

	struct nouveau_fence *next;

	uint32_t sequence;
	int emitted;
	int signalled;
};
#define nouveau_fence(n) ((struct nouveau_fence_priv *)(n))

NOUVEAU_PRIVATE int
nouveau_fence_new(struct nouveau_channel *, struct nouveau_fence **);

NOUVEAU_PRIVATE int
nouveau_fence_ref(struct nouveau_fence *, struct nouveau_fence **);

NOUVEAU_PRIVATE void
nouveau_fence_emit(struct nouveau_fence *);

NOUVEAU_PRIVATE int
nouveau_fence_wait(struct nouveau_fence **);

NOUVEAU_PRIVATE void
nouveau_fence_flush(struct nouveau_channel *);

#define NOUVEAU_PUSHBUF_MAX_BUFFERS 1024
#define NOUVEAU_PUSHBUF_MAX_RELOCS 1024
struct nouveau_pushbuf_priv {
	struct nouveau_pushbuf base;

	unsigned *pushbuf;
	unsigned  size;

	struct drm_nouveau_gem_pushbuf_bo *buffers;
	unsigned nr_buffers;
	struct drm_nouveau_gem_pushbuf_reloc *relocs;
	unsigned nr_relocs;
};
#define nouveau_pushbuf(n) ((struct nouveau_pushbuf_priv *)(n))

#define pbbo_to_ptr(o) ((uint64_t)(unsigned long)(o))
#define ptr_to_pbbo(h) ((struct nouveau_pushbuf_bo *)(unsigned long)(h))
#define pbrel_to_ptr(o) ((uint64_t)(unsigned long)(o))
#define ptr_to_pbrel(h) ((struct nouveau_pushbuf_reloc *)(unsigned long)(h))
#define bo_to_ptr(o) ((uint64_t)(unsigned long)(o))
#define ptr_to_bo(h) ((struct nouveau_bo_priv *)(unsigned long)(h))

NOUVEAU_PRIVATE int
nouveau_pushbuf_init(struct nouveau_channel *);

NOUVEAU_PRIVATE int
nouveau_pushbuf_flush(struct nouveau_channel *, unsigned min);

NOUVEAU_PRIVATE int
nouveau_pushbuf_emit_reloc(struct nouveau_channel *, void *ptr,
			   struct nouveau_bo *, uint32_t data, uint32_t flags,
			   uint32_t vor, uint32_t tor);

struct nouveau_channel_priv {
	struct nouveau_channel base;

	struct drm_nouveau_channel_alloc drm;

	void     *notifier_block;

	struct nouveau_fence *fence_head;
	struct nouveau_fence *fence_tail;
	uint32_t fence_sequence;
	/* NV04 hackery */
	struct nouveau_grobj *fence_grobj;
	struct nouveau_notifier *fence_ntfy;

	struct nouveau_pushbuf_priv pb;

	unsigned user_charge;
};
#define nouveau_channel(n) ((struct nouveau_channel_priv *)(n))

NOUVEAU_PRIVATE int
nouveau_channel_alloc(struct nouveau_device *, uint32_t fb, uint32_t tt,
		      struct nouveau_channel **);

NOUVEAU_PRIVATE void
nouveau_channel_free(struct nouveau_channel **);

struct nouveau_grobj_priv {
	struct nouveau_grobj base;
};
#define nouveau_grobj(n) ((struct nouveau_grobj_priv *)(n))

NOUVEAU_PRIVATE int nouveau_grobj_alloc(struct nouveau_channel *, uint32_t handle,
			       int class, struct nouveau_grobj **);
NOUVEAU_PRIVATE int nouveau_grobj_ref(struct nouveau_channel *, uint32_t handle,
			     struct nouveau_grobj **);
NOUVEAU_PRIVATE void nouveau_grobj_free(struct nouveau_grobj **);


struct nouveau_notifier_priv {
	struct nouveau_notifier base;

	struct drm_nouveau_notifierobj_alloc drm;
	volatile void *map;
};
#define nouveau_notifier(n) ((struct nouveau_notifier_priv *)(n))

NOUVEAU_PRIVATE int
nouveau_notifier_alloc(struct nouveau_channel *, uint32_t handle, int count,
		       struct nouveau_notifier **);

NOUVEAU_PRIVATE void
nouveau_notifier_free(struct nouveau_notifier **);

NOUVEAU_PRIVATE void
nouveau_notifier_reset(struct nouveau_notifier *, int id);

NOUVEAU_PRIVATE uint32_t
nouveau_notifier_status(struct nouveau_notifier *, int id);

NOUVEAU_PRIVATE uint32_t
nouveau_notifier_return_val(struct nouveau_notifier *, int id);

NOUVEAU_PRIVATE int
nouveau_notifier_wait_status(struct nouveau_notifier *, int id, int status,
			     int timeout);

struct nouveau_bo_priv {
	struct nouveau_bo base;
	int refcount;

	/* Buffer configuration + usage hints */
	unsigned flags;
	unsigned size;
	unsigned align;
	int user;

	/* Tracking */
	struct drm_nouveau_gem_pushbuf_bo *pending;
	struct nouveau_channel *pending_channel;
	struct nouveau_fence *fence;
	struct nouveau_fence *wr_fence;

	/* Userspace object */
	void *sysmem;

	/* Kernel object */
	uint32_t global_handle;
	unsigned handle;
	void *map;

	/* Last known information from kernel on buffer status */
	int pinned;
	uint64_t offset;
	uint32_t domain;
};
#define nouveau_bo(n) ((struct nouveau_bo_priv *)(n))

NOUVEAU_PRIVATE int
nouveau_bo_init(struct nouveau_device *);

NOUVEAU_PRIVATE void
nouveau_bo_takedown(struct nouveau_device *);

NOUVEAU_PRIVATE int
nouveau_bo_new(struct nouveau_device *, uint32_t flags, int align, int size,
	       struct nouveau_bo **);

NOUVEAU_PRIVATE int
nouveau_bo_user(struct nouveau_device *, void *ptr, int size,
		struct nouveau_bo **);

NOUVEAU_PRIVATE int
nouveau_bo_handle_get(struct nouveau_bo *, uint32_t *);

NOUVEAU_PRIVATE int
nouveau_bo_handle_ref(struct nouveau_device *, uint32_t handle,
		      struct nouveau_bo **);

NOUVEAU_PRIVATE int
nouveau_bo_ref(struct nouveau_bo *, struct nouveau_bo **);

NOUVEAU_PRIVATE int
nouveau_bo_map(struct nouveau_bo *, uint32_t flags);

NOUVEAU_PRIVATE void
nouveau_bo_unmap(struct nouveau_bo *);

NOUVEAU_PRIVATE int
nouveau_bo_pin(struct nouveau_bo *, uint32_t flags);

NOUVEAU_PRIVATE void
nouveau_bo_unpin(struct nouveau_bo *);

NOUVEAU_PRIVATE uint64_t
nouveau_bo_get_drm_map(struct nouveau_bo *);

NOUVEAU_PRIVATE int
nouveau_resource_init(struct nouveau_resource **heap, unsigned start,
		      unsigned size);

NOUVEAU_PRIVATE int
nouveau_resource_alloc(struct nouveau_resource *heap, int size, void *priv,
		       struct nouveau_resource **);

NOUVEAU_PRIVATE void
nouveau_resource_free(struct nouveau_resource **);

NOUVEAU_PRIVATE struct drm_nouveau_gem_pushbuf_bo *
nouveau_bo_emit_buffer(struct nouveau_channel *, struct nouveau_bo *);

#endif
