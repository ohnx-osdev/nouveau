/*
 * Copyright 2007 Ben Skeggs
 * Copyright 2007 Stephane Marchesin
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

#include <errno.h>
#include "nv_include.h"
#include "nvreg.h"

void NVSync(ScrnInfoPtr pScrn)
{
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_channel *chan = pNv->chan;
	struct nouveau_grobj *gr = pNv->Nv2D ? pNv->Nv2D : pNv->NvImageBlit;
	int timeout = 2000;

	if (pNv->NoAccel)
		return;

	/* Wait for nvchannel to go completely idle */
	nouveau_notifier_reset(pNv->notify0, 0);
	BEGIN_RING(chan, gr, 0x104, 1);
	OUT_RING  (chan, 0);
	BEGIN_RING(chan, gr, 0x100, 1);
	OUT_RING  (chan, 0);
	FIRE_RING (chan);
	if (nouveau_notifier_wait_status(pNv->notify0, 0,
					 NV_NOTIFY_STATE_STATUS_COMPLETED,
					 timeout)) {
		if (!pNv->LockedUp) {
			pNv->LockedUp = TRUE;
			FatalError("GPU hang\n");
		}
	}
}

Bool
NVInitDma(ScrnInfoPtr pScrn)
{
	NVPtr pNv = NVPTR(pScrn);
	int ret;

	ret = nouveau_channel_alloc(pNv->dev, NvDmaFB, NvDmaTT, &pNv->chan);
	if (ret) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Error creating GPU channel: %d\n", ret);
		return FALSE;
	}

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Opened GPU channel %d\n", pNv->chan->id);

	return TRUE;
}

