/*
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Keith Packard makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL KEITH PACKARD BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86.h"
#include "os.h"
#include "mibank.h"
#include "globals.h"
#include "xf86.h"
#include "xf86Priv.h"
#include "xf86DDC.h"
#include "mipointer.h"
#include "windowstr.h"
#include <randrstr.h>
#include <X11/extensions/render.h>

#include "nv_xf86Crtc.h"
#include "nv_randr.h"
#include "nv_include.h"


#define CRTC_INDEX 0x3d4
#define CRTC_DATA 0x3d5

static void NVWriteCrtc(xf86CrtcPtr crtc, CARD8 index, CARD8 value)
{
  NVCrtcPrivatePtr nv_crtc = crtc->driver_private;

  NV_WR08(nv_crtc->pCRTCReg, CRTC_INDEX, index);
  NV_WR08(nv_crtc->pCRTCReg, CRTC_DATA, value);
}

static CARD8 NVReadCrtc(xf86CrtcPtr crtc, CARD8 index)
{
  NVCrtcPrivatePtr nv_crtc = crtc->driver_private;

  NV_WR08(nv_crtc->pCRTCReg, CRTC_INDEX, index);
  return NV_RD08(nv_crtc->pCRTCReg, CRTC_DATA);
}

static void
nv_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
     ScrnInfoPtr pScrn = crtc->scrn;
     NVPtr pI830 = NVPTR(pScrn);
     NVCrtcPrivatePtr nv_crtc = crtc->driver_private;
     unsigned char crtc1A;
     vgaHWPtr hwp = VGAHWPTR(pScrn);
     int ret;

     crtc1A = NVReadCrtc(crtc, 0x1A) & ~0xC0;
     switch(mode) {
     case DPMSModeStandby:
       crtc1A |= 0x80;
       break;
     case DPMSModeSuspend:
       crtc1A |= 0x40;
       break;
     case DPMSModeOff:
       crtc1A |= 0xC0;
       break;
     case DPMSModeOn:
     default:
       break;
     }
     
     NVWriteCrtc(crtc, 0x1A, crtc1A);
}

static Bool
nv_crtc_mode_fixup(xf86CrtcPtr crtc, DisplayModePtr mode,
		     DisplayModePtr adjusted_mode)
{
    return TRUE;
}

/**
 * Sets up registers for the given mode/adjusted_mode pair.
 *
 * The clocks, CRTCs and outputs attached to this CRTC must be off.
 *
 * This shouldn't enable any clocks, CRTCs, or outputs, but they should
 * be easily turned on/off after this.
 */
static void
nv_crtc_mode_set(xf86CrtcPtr crtc, DisplayModePtr mode,
		 DisplayModePtr adjusted_mode)
{
    ScrnInfoPtr pScrn = crtc->scrn;
    xf86CrtcConfigPtr   xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    NVPtr pI830 = NVPTR(pScrn);
    NVCrtcPrivatePtr nv_crtc = crtc->driver_private;

}


static const xf86CrtcFuncsRec nv_crtc_funcs = {
    .dpms = nv_crtc_dpms,
    .save = NULL, /* XXX */
    .restore = NULL, /* XXX */
    .mode_fixup = nv_crtc_mode_fixup,
    .mode_set = nv_crtc_mode_set,
    .destroy = NULL, /* XXX */
};

void
nv_crtc_init(ScrnInfoPtr pScrn, int crtc_num)
{
    NVPtr pNv = NVPTR(pScrn);
    xf86CrtcPtr crtc;
    NVCrtcPrivatePtr nv_crtc;

    crtc = xf86CrtcCreate (pScrn, &nv_crtc_funcs);
    if (crtc == NULL)
	return;

    nv_crtc = xnfcalloc (sizeof (NVCrtcPrivateRec), 1);
    nv_crtc->crtc = crtc_num;

    if (crtc_num == 0)
      nv_crtc->pCRTCReg = pNv->PCIO0;
    else
      nv_crtc->pCRTCReg = pNv->PCIO1;

    crtc->driver_private = nv_crtc;
}
