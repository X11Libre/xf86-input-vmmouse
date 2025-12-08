/*
 * Copyright 1990,91 by Thomas Roell, Dinkelscherben, Germany.
 * Copyright 1993 by David Dawes <dawes@xfree86.org>
 * Copyright 2002 by SuSE Linux AG, Author: Egbert Eich
 * Copyright 1994-2002 by The XFree86 Project, Inc.
 * Copyright 2002 by Paul Elliott
 * Copyright 2002-2006 by VMware, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the names of copyright holders not be
 * used in advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.  The copyright holders
 * make no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without express or
 * implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

/*
 * vmmouse.c --
 *
 * 	This is a modified version of the mouse input driver
 * 	provided in Xserver/hw/xfree86/input/mouse/mouse.c
 *
 *      Although all data is read using the vmmouse protocol, notification
 *      is still done through the PS/2 port, so all the basic code for
 *      interacting with the port is retained.
 *
 */


/*****************************************************************************
 *	Standard Headers
 ****************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdbool.h>
#include <X11/X.h>
#include <X11/Xproto.h>

#include "xf86.h"

#ifdef XINPUT
#include <X11/extensions/XI.h>
#include <X11/extensions/XIproto.h>
#include "extnsionst.h"
#include "extinit.h"
#else
#include "inputstr.h"
#endif

#include "xf86Xinput.h"
#include "xf86_OSproc.h"
#include "xf86OSmouse.h"
#include "xf86Priv.h"
#include "compiler.h"

#include <xserver-properties.h>
#include "exevents.h"

#include "xisb.h"
#include "mipointer.h"

/*****************************************************************************
 *	Local Headers
 ****************************************************************************/
#include "vmmouse_client.h"

/*
 * This is the only way I know to turn a #define of an integer constant into
 * a constant string.
 */
#define VMW_INNERSTRINGIFY(s) #s
#define VMW_STRING(str) VMW_INNERSTRINGIFY(str)

/*
 * Version constants
 */
#define VMMOUSE_DRIVER_VERSION \
   (PACKAGE_VERSION_MAJOR * 65536 + PACKAGE_VERSION_MINOR * 256 + PACKAGE_VERSION_PATCHLEVEL)
#define VMMOUSE_DRIVER_VERSION_STRING \
    VMW_STRING(PACKAGE_VERSION_MAJOR) "." VMW_STRING(PACKAGE_VERSION_MINOR) \
    "." VMW_STRING(PACKAGE_VERSION_PATCHLEVEL)

/*
 * Standard four digit version string expected by VMware Tools installer.
 * As the driver's version is only  {major, minor, patchlevel},
 * The fourth digit may describe the commit number relative to the
 * last version tag as output from `git describe`
 */
#ifdef __GNUC__
#ifdef VMW_SUBPATCH
const char vm_mouse_version[] __attribute__((section(".modinfo"),unused)) =
    "version=" VMMOUSE_DRIVER_VERSION_STRING "." VMW_STRING(VMW_SUBPATCH);
#else
const char vm_mouse_version[] __attribute__((section(".modinfo"),unused)) =
    "version=" VMMOUSE_DRIVER_VERSION_STRING ".0";
#endif /*VMW_SUBPATCH*/
#endif


/*****************************************************************************
 *	static function header
 ****************************************************************************/
static int VMMousePreInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags);
static void VMMouseUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags);
static void MouseCommonOptions(InputInfoPtr pInfo);
static void GetVMMouseMotionEvent(InputInfoPtr pInfo);
static void VMMousePostEvent(InputInfoPtr pInfo, int buttons, int dx, int dy, int dz, int dw);
static void VMMouseDoPostEvent(InputInfoPtr pInfo, int buttons, int dx, int dy);
static bool VMMouseDeviceControl(DeviceIntPtr device, int mode);
static int  VMMouseControlProc(InputInfoPtr pInfo, xDeviceCtl * control);
static void VMMouseReadInput(InputInfoPtr pInfo);
static int  VMMouseSwitchMode(ClientPtr client, DeviceIntPtr dev, int mode);
static void MouseCtrl(DeviceIntPtr device, PtrCtrl *ctrl);

/******************************************************************************
 *		Definitions
 *****************************************************************************/
typedef struct {
   int                 screenNum;
   bool                vmmouseAvailable;
   VMMOUSE_INPUT_DATA  vmmousePrevInput;
   bool                isCurrRelative;
   bool                absoluteRequested;
} VMMousePrivRec, *VMMousePrivPtr;

InputDriverRec VMMOUSE = {
   1,
   "vmmouse",
   NULL,
   VMMousePreInit,
   VMMouseUnInit,
   NULL
};

static char reverseMap[32] = { 0,  4,  2,  6,  1,  5,  3,  7,
			       8, 12, 10, 14,  9, 13, 11, 15,
			      16, 20, 18, 22, 17, 21, 19, 23,
			      24, 28, 26, 30, 25, 29, 27, 31};

#define reverseBits(map, b)	(((b) & ~0x0f) | map[(b) & 0x0f])

static int
VMMouseInitPassthru(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
   InputAttributes *attrs = NULL;
   InputOption *input_options = NULL;
   pointer options;
   DeviceIntPtr dev;
   int rc;

   options = xf86OptionListDuplicate(pInfo->options);
   options = xf86ReplaceStrOption(options, "Driver", "mouse");

   while(options) {
      input_options = input_option_new(input_options,
                                       xf86OptionName(options),
                                       xf86OptionValue(options));
      options = xf86NextOption(options);
   }

   rc = NewInputDeviceRequest(input_options, attrs, &dev);

   input_option_free_list(&input_options);

   return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * VMMousePreInit --
 *	This function collect all the information that is necessary to
 *	determine the configuration of the hardware and to prepare the
 *	device for being used
 *
 * Results:
 * 	An InputInfoPtr object which points to vmmouse's information,
 *	if the absolute pointing device available
 *	Otherwise, an InputInfoPtr of regular mouse
 *
 * Side effects:
 * 	VMMouse was initialized with necessary information
 *
 *----------------------------------------------------------------------
 */

static int
VMMousePreInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
   MouseDevPtr pMse = NULL;
   VMMousePrivPtr mPriv = NULL;
   int rc = Success;

   /* Enable hardware access. */
   if (!xorgHWAccess) {
      if (xf86EnableIO())
          xorgHWAccess = true;
      else {
          rc = BadValue;
          goto error;
      }
   }

   /* For ABI < 12, we need to return the wrapped driver's pInfo (see
    * above). ABI 12, we call NIDR and are done */
   if (!VMMouseClient_Enable()) {
      xf86Msg(X_ERROR, "VMWARE(0): vmmouse enable failed\n");
      return VMMouseInitPassthru(drv, pInfo, flags);
   } else {
      xf86Msg(X_INFO, "VMWARE(0): vmmouse is available\n");
      VMMouseClient_Disable();
   }

   mPriv = calloc (1, sizeof (VMMousePrivRec));

   if (!mPriv) {
      rc = BadAlloc;
      goto error;
   }

   mPriv->absoluteRequested = false;
   mPriv->vmmouseAvailable = true;

   /* Settup the pInfo */
   pInfo->type_name = XI_MOUSE;
   pInfo->device_control = VMMouseDeviceControl;
   pInfo->read_input = VMMouseReadInput;
   pInfo->control_proc = VMMouseControlProc;
   pInfo->switch_mode = VMMouseSwitchMode;

   /* Allocate the MouseDevRec and initialise it. */
   if (!(pMse = calloc(sizeof(MouseDevRec), 1))) {
      rc = BadAlloc;
      goto error;
   }

   pInfo->private = pMse;
   pMse->Ctrl = MouseCtrl;
   pMse->PostEvent = VMMousePostEvent;
   pMse->CommonOptions = MouseCommonOptions;
   pMse->mousePriv = mPriv;


   /* Check if the device can be opened. */
   pInfo->fd = xf86OpenSerial(pInfo->options);
   if (pInfo->fd == -1) {
      if (xf86GetAllowMouseOpenFail())
	 xf86Msg(X_WARNING, "%s: cannot open input device\n", pInfo->name);
      else {
	 xf86Msg(X_ERROR, "%s: cannot open input device\n", pInfo->name);
	 rc = BadValue;
	 goto error;
      }
   }
   xf86CloseSerial(pInfo->fd);
   pInfo->fd = -1;

   /* Process the options */
   pMse->CommonOptions(pInfo);

   /* set up the current screen num */
   mPriv->screenNum = xf86SetIntOption(pInfo->options, "ScreenNumber", 0);

   return Success;

error:
   pInfo->private = NULL;
   if (mPriv)
      free(mPriv);
   if (pMse)
      free(pMse);

   return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * MouseCtrl --
 *     Alter the control paramters for the mouse.
 *
 * Results:
 * 	None
 *
 * Side effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */

static void
MouseCtrl(DeviceIntPtr device, PtrCtrl *ctrl)
{
    InputInfoPtr pInfo;
    MouseDevPtr pMse;

    pInfo = device->public.devicePrivate;
    pMse = pInfo->private;

#ifdef EXTMOUSEDEBUG
    xf86Msg(X_INFO, "VMMOUSE(0): MouseCtrl pMse=%p\n", pMse);
#endif

    pMse->num       = ctrl->num;
    pMse->den       = ctrl->den;
    pMse->threshold = ctrl->threshold;
}


/*
 *----------------------------------------------------------------------
 *
 * VMMouseDoPostEvent --
 *	Post the mouse button event and mouse motion event to Xserver
 *
 * Results:
 * 	None
 *
 * Side effects:
 * 	Mouse location and button status was updated
 *
 *----------------------------------------------------------------------
 */

static void
VMMouseDoPostEvent(InputInfoPtr pInfo, int buttons, int dx, int dy)
{
    MouseDevPtr pMse;
    VMMousePrivPtr mPriv;
    int truebuttons;
    int id, change;
    bool mouseMoved = false;

    pMse = pInfo->private;
    mPriv = (VMMousePrivPtr)pMse->mousePriv;

    /*
     * The following truebuttons/reverseBits and lastButtons are
     * used to compare the current buttons and the previous buttons
     * to find the button changes during two mouse events
     */
    truebuttons = buttons;

    buttons = reverseBits(reverseMap, buttons);

    if (mPriv->isCurrRelative) {
       mouseMoved = dx || dy;
    } else {
       mouseMoved = (dx != mPriv->vmmousePrevInput.X) ||
                    (dy != mPriv->vmmousePrevInput.Y) ||
                    (mPriv->vmmousePrevInput.Flags & VMMOUSE_MOVE_RELATIVE);
    }
    if (mouseMoved) {
        xf86PostMotionEvent(pInfo->dev, !mPriv->isCurrRelative, 0, 2, dx, dy);
    }

    if (truebuttons != pMse->lastButtons) {
       change = buttons ^ reverseBits(reverseMap, pMse->lastButtons);
       while (change) {
	  id = ffs(change);
	  change &= ~(1 << (id - 1));
	  xf86PostButtonEvent(pInfo->dev, 0, id,
			      (buttons & (1 << (id - 1))), 0, 0);
       }
       pMse->lastButtons = truebuttons;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * VMMousePostEvent --
 *	Prepare the mouse status according to the Z axis mapping
 *	before we post the event to Xserver
 *
 * Results:
 * 	None
 *
 * Side effects:
 * 	Buttons was updated according to Z axis mapping
 *
 *----------------------------------------------------------------------
 */

static void
VMMousePostEvent(InputInfoPtr pInfo, int buttons, int dx, int dy, int dz, int dw)
{
    MouseDevPtr pMse;
    int zbutton = 0;
    VMMousePrivPtr mPriv;

    pMse = pInfo->private;
    mPriv = (VMMousePrivPtr)pMse->mousePriv;
    /* Map the Z axis movement. */
    /* XXX Could this go in the conversion_proc? */
    switch (pMse->negativeZ) {
    case MSE_NOZMAP:	/* do nothing */
	break;
    case MSE_MAPTOX:
	if (dz != 0) {
	   if(mPriv->isCurrRelative)
	      dx = dz;
	   else
	      dx += dz;
	    dz = 0;
	}
	break;
    case MSE_MAPTOY:
	if (dz != 0) {
	   if(mPriv->isCurrRelative)
	      dy = dz;
	   else
	      dy += dz;
	    dz = 0;
	}
	break;
    default:	/* buttons */
	buttons &= ~(pMse->negativeZ | pMse->positiveZ
		   | pMse->negativeW | pMse->positiveW);
	if (dw < 0 || dz < -1) {
	    zbutton = pMse->negativeW;
	}
	else if (dz < 0) {
	    zbutton = pMse->negativeZ;
	}
	else if (dw > 0 || dz > 1) {
	    zbutton = pMse->positiveW;
	}
	else if (dz > 0) {
	    zbutton = pMse->positiveZ;
	}
	buttons |= zbutton;
	dz = 0;
	break;
    }

    VMMouseDoPostEvent(pInfo, buttons, dx, dy);

    /*
     * If dz has been mapped to a button `down' event, we need to cook up
     * a corresponding button `up' event.
     */
    if (zbutton) {
	buttons &= ~zbutton;
	if(mPriv->isCurrRelative)
	   VMMouseDoPostEvent(pInfo, buttons, 0, 0);
	else
	   VMMouseDoPostEvent(pInfo, buttons, dx, dy);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * FlushButtons --
 *
 * 	FlushButtons -- reset button states.
 *
 * Results:
 * 	None
 *
 * Side effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */

static void
FlushButtons(MouseDevPtr pMse)
{
    pMse->lastButtons = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * MouseCommonOptions --
 *	Process acceptable mouse options. Currently we only process
 *	"Buttons" and "ZAxisMapping" options.
 *	More options can be added later on
 *
 * Results:
 * 	None
 *
 * Side effects:
 * 	The buttons was setup according to the options
 *
 *----------------------------------------------------------------------
 */

static void
MouseCommonOptions(InputInfoPtr pInfo)
{
   MouseDevPtr pMse;
   char *s;

   pMse = pInfo->private;

   pMse->buttons = xf86SetIntOption(pInfo->options, "Buttons", 0);
   if (!pMse->buttons) {
      pMse->buttons = MSE_DFLTBUTTONS;
   }

   /*
    * "emulate3Buttons" and "Drag Lock" is not supported
    */

   /*
    * Process option for ZAxisMapping
    */
   s = xf86SetStrOption(pInfo->options, "ZAxisMapping", "4 5");
   if (s) {
      int b1 = 0, b2 = 0, b3 = 0, b4 = 0;
      char *msg = NULL;

      if (!xf86NameCmp(s, "x")) {
	 pMse->negativeZ = pMse->positiveZ = MSE_MAPTOX;
	 pMse->negativeW = pMse->positiveW = MSE_MAPTOX;
	 msg = strdup("X axis");
      } else if (!xf86NameCmp(s, "y")) {
	 pMse->negativeZ = pMse->positiveZ = MSE_MAPTOY;
	 pMse->negativeW = pMse->positiveW = MSE_MAPTOY;
	 msg = strdup("Y axis");
      } else if (sscanf(s, "%d %d %d %d", &b1, &b2, &b3, &b4) >= 2 &&
		 b1 > 0 && b1 <= MSE_MAXBUTTONS &&
		 b2 > 0 && b2 <= MSE_MAXBUTTONS) {
	 msg = strdup("buttons XX and YY");
	 if (msg)
	    sprintf(msg, "buttons %d and %d", b1, b2);
	 pMse->negativeZ = pMse->negativeW = 1 << (b1-1);
	 pMse->positiveZ = pMse->positiveW = 1 << (b2-1);
	 if (b1 > pMse->buttons) pMse->buttons = b1;
	 if (b2 > pMse->buttons) pMse->buttons = b2;

	 /*
	  * Option "ZAxisMapping" "N1 N2 N3 N4" not supported
	  */
	 pMse->negativeW = pMse->positiveW = MSE_NOZMAP;
      } else {
	 pMse->negativeZ = pMse->positiveZ = MSE_NOZMAP;
	 pMse->negativeW = pMse->positiveW = MSE_NOZMAP;
      }
      if (msg) {
	 xf86Msg(X_CONFIG, "%s: ZAxisMapping: %s\n", pInfo->name, msg);
	 free(msg);
      } else {
	 xf86Msg(X_WARNING, "%s: Invalid ZAxisMapping value: \"%s\"\n",
		 pInfo->name, s);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VMMouseUnInit --
 * 	This function was supposed to be called by Xserver to do Un-Init.
 *	But it was unused now
 *
 * Results:
 * 	None
 *
 * Side effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */

static void
VMMouseUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
   MouseDevPtr pMse = pInfo->private;

   xf86Msg(X_INFO, "VMWARE(0): VMMouseUnInit\n");

   if (pMse) {
       VMMousePrivPtr mPriv = (VMMousePrivPtr)pMse->mousePriv;
       free(mPriv);
   }

   xf86DeleteInput(pInfo, flags);
}


/*
 *----------------------------------------------------------------------
 *
 * VMMouseDeviceControl --
 * 	This function was called by Xserver during DEVICE_INIT, DEVICE_ON,
 *	DEVICE_OFF and DEVICE_CLOSE phase
 *
 * Results:
 * 	true, if sucessful
 *	false, if failed
 *
 * Side effects:
 * 	Absolute pointing device is enabled during DEVICE_ON
 *	Absolute pointing device is disabled during DEVICE_OFF
 *	and DEVICE_CLOSE
 *
 *----------------------------------------------------------------------
 */

static bool
VMMouseDeviceControl(DeviceIntPtr device, int mode)
{
   InputInfoPtr pInfo;
   MouseDevPtr pMse;
   unsigned char map[MSE_MAXBUTTONS + 1];
   int i;
   Atom btn_labels[MSE_MAXBUTTONS] = {0};
   Atom axes_labels[2] = { 0, 0 };

   pInfo = device->public.devicePrivate;
   pMse = pInfo->private;
   pMse->device = device;

   switch (mode){
   case DEVICE_INIT:
      device->public.on = false;
      /*
       * [KAZU-241097] We don't know exactly how many buttons the
       * device has, so setup the map with the maximum number.
       */
      for (i = 0; i < MSE_MAXBUTTONS; i++)
	 map[i + 1] = i + 1;
      btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
      btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
      btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
      btn_labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
      btn_labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
      btn_labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
      btn_labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
      /* other buttons are unknown */

      axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_X);
      axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_Y);

      InitPointerDeviceStruct((DevicePtr)device, map,
			      min(pMse->buttons, MSE_MAXBUTTONS),
				btn_labels,
                                pMse->Ctrl,
                                GetMotionHistorySize(), 2
				, axes_labels
                                );

      /* X valuator */
      xf86InitValuatorAxisStruct(device, 0,
				axes_labels[0],
				0, 65535, 10000, 0, 10000
                                , Absolute
                                );
      xf86InitValuatorDefaults(device, 0);
      /* Y valuator */
      xf86InitValuatorAxisStruct(device, 1,
				axes_labels[1],
				0, 65535, 10000, 0, 10000
                                , Absolute
                                );
      xf86InitValuatorDefaults(device, 1);

      xf86Msg(X_INFO, "VMWARE(0): VMMOUSE DEVICE_INIT\n");
#ifdef EXTMOUSEDEBUG
      xf86Msg(X_INFO, "assigning %p atom=%d name=%s\n", device, pInfo->atom,
	     pInfo->name);
#endif
      break;

   case DEVICE_ON:
      xf86Msg(X_INFO, "VMWARE(0): VMMOUSE DEVICE_ON\n");
      pInfo->fd = xf86OpenSerial(pInfo->options);
      if (pInfo->fd == -1)
	 xf86Msg(X_WARNING, "%s: cannot open input device\n", pInfo->name);
      else {
	 pMse->buffer = XisbNew(pInfo->fd, 64);
	 if (!pMse->buffer) {
	    xf86CloseSerial(pInfo->fd);
	    pInfo->fd = -1;
	 } else {
	    VMMousePrivPtr mPriv = (VMMousePrivPtr)pMse->mousePriv;
	    if (mPriv != NULL) {
	       /*
		* enable absolute pointing device here
		*/
	       if (!VMMouseClient_Enable()) {
		  xf86Msg(X_ERROR, "VMWARE(0): vmmouse enable failed\n");
		  mPriv->vmmouseAvailable = false;
		  device->public.on = false;
		  return false;
	       } else {
		  mPriv->vmmouseAvailable = true;
		  xf86Msg(X_INFO, "VMWARE(0): vmmouse enabled\n");
	       }
	    }
	    xf86FlushInput(pInfo->fd);
	    xf86AddEnabledDevice(pInfo);
	 }
      }
      pMse->lastButtons = 0;
      device->public.on = true;
      FlushButtons(pMse);
      break;
   case DEVICE_OFF:
   case DEVICE_CLOSE:
      xf86Msg(X_INFO, "VMWARE(0): VMMOUSE DEVICE_OFF/CLOSE\n");

      if (pInfo->fd != -1) {
	 VMMousePrivPtr mPriv = (VMMousePrivPtr)pMse->mousePriv;
	 if( mPriv->vmmouseAvailable ) {
	    VMMouseClient_Disable();
            mPriv->vmmouseAvailable = false;
            mPriv->absoluteRequested = false;
	 }

	 xf86RemoveEnabledDevice(pInfo);
	 if (pMse->buffer) {
	    XisbFree(pMse->buffer);
	    pMse->buffer = NULL;
	 }
	 xf86CloseSerial(pInfo->fd);
	 pInfo->fd = -1;
      }
      device->public.on = false;
      usleep(300000);
      break;

   case  DEVICE_ABORT:
      if (pInfo->fd != -1) {
	 VMMousePrivPtr mPriv = (VMMousePrivPtr)pMse->mousePriv;
	 if( mPriv->vmmouseAvailable )
	    VMMouseClient_Disable();
         break;
      }
   }

   return Success;
}


/*
 *----------------------------------------------------------------------
 *
 * VMMouseReadInput --
 * 	This function was called by Xserver when there is data available
 *	in the input device
 *
 * Results:
 * 	None
 *
 * Side effects:
 * 	Input data in regular PS/2 fd was cleared
 *	Real mouse data was read from the absolute pointing device
 *	and posted to Xserver
 *
 *----------------------------------------------------------------------
 */

static void
VMMouseReadInput(InputInfoPtr pInfo)
{
   MouseDevPtr pMse;
   VMMousePrivPtr mPriv;
   int c;
   int len = 0;

   pMse = pInfo->private;
   mPriv = pMse->mousePriv;

   if (!mPriv->absoluteRequested) {
      /*
       * We can request for absolute mode, but it depends on
       * host whether it will send us absolute or relative
       * position.
       */
      VMMouseClient_RequestAbsolute();
      mPriv->absoluteRequested = true;
      LogMessageVerbSigSafe(X_INFO, -1, "VMWARE(0): vmmouse enable absolute mode\n");
   }

   /*
    * First read the bytes in input device to clear the regular PS/2 fd so
    * we don't get called again.
    */
   /*
    * Set blocking to -1 on the first call because we know there is data to
    * read. Xisb automatically clears it after one successful read so that
    * succeeding reads are preceeded by a select with a 0 timeout to prevent
    * read from blocking indefinitely.
    */
   XisbBlockDuration(pMse->buffer, -1);
   while ((c = XisbRead(pMse->buffer)) >= 0) {
      len++;
      /*
       * regular PS packet consists of 3 bytes
       * We read 3 bytes to drain the PS/2 packet
       */
      if(len < 3) continue;
      len = 0;
      /*
       * Now get the real data from absolute pointing device
       */
      GetVMMouseMotionEvent(pInfo);
   }
   /*
    * There maybe still vmmouse data available
    */
   GetVMMouseMotionEvent(pInfo);
}


/*
 *----------------------------------------------------------------------
 *
 * GetVMMouseMotionEvent --
 * 	Read all the mouse data available from the absolute
 * 	pointing device	and post it to the Xserver
 *
 * Results:
 * 	None
 *
 * Side effects:
 *	Real mouse data was read from the absolute pointing
 *	device and posted to Xserver
 *
 *----------------------------------------------------------------------
 */

static void
GetVMMouseMotionEvent(InputInfoPtr pInfo){
   MouseDevPtr pMse;
   VMMousePrivPtr mPriv;
   int buttons, dx, dy, dz, dw;
   VMMOUSE_INPUT_DATA  vmmouseInput;
   int numPackets;

   pMse = pInfo->private;
   mPriv = (VMMousePrivPtr)pMse->mousePriv;
   while((numPackets = VMMouseClient_GetInput(&vmmouseInput))){
      int ps2Buttons = 0;
      if (numPackets == VMMOUSE_ERROR) {
         VMMouseClient_Disable();
         VMMouseClient_Enable();
         VMMouseClient_RequestAbsolute();
         LogMessageVerbSigSafe(X_INFO, -1, "VMWARE(0): re-requesting absolute mode after reset\n");
         break;
      }

      if(vmmouseInput.Buttons & VMMOUSE_MIDDLE_BUTTON)
	 ps2Buttons |= 0x04; 			/* Middle*/
      if(vmmouseInput.Buttons & VMMOUSE_RIGHT_BUTTON)
	 ps2Buttons |= 0x02; 			/* Right*/
      if(vmmouseInput.Buttons & VMMOUSE_LEFT_BUTTON)
	 ps2Buttons |= 0x01; 			/* Left*/

      buttons = (ps2Buttons & 0x04) >> 1 |	/* Middle */
	 (ps2Buttons & 0x02) >> 1 |       	/* Right */
	 (ps2Buttons & 0x01) << 2;       	/* Left */

      dx = vmmouseInput.X;
      dy = vmmouseInput.Y;
      dz = (char)vmmouseInput.Z;
      dw = 0;
      /*
       * Get the per package relative or absolute information.
       */
      mPriv->isCurrRelative = vmmouseInput.Flags & VMMOUSE_MOVE_RELATIVE;
      /* post an event */
      pMse->PostEvent(pInfo, buttons, dx, dy, dz, dw);
      mPriv->vmmousePrevInput = vmmouseInput;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VMMouseControlProc --
 *	This function is unused
 *
 * Results:
 * 	None
 *
 * Side effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */

static int
VMMouseControlProc(InputInfoPtr pInfo, xDeviceCtl * control)
{
   xf86Msg(X_INFO, "VMWARE(0): VMMouseControlProc\n");
   return (Success);
}


/*
 *----------------------------------------------------------------------
 *
 *  VMMouseSwitchProc --
 *	This function is unused
 *
 * Results:
 * 	None
 *
 * Side effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */

static int
VMMouseSwitchMode(ClientPtr client, DeviceIntPtr dev, int mode)
{
   xf86Msg(X_INFO, "VMWARE(0): VMMouseSwitchMode\n");
   return (Success);
}


/*
 *----------------------------------------------------------------------
 *
 * VMMouseConvertProc  --
 * 	This function was called by Xserver to convert valuators to X and Y
 *
 * Results:
 * 	true
 *
 * Side effects:
 * 	X and Y was converted according to current Screen dimension
 *
 *----------------------------------------------------------------------
 */

#ifdef XFree86LOADER

/*
 *----------------------------------------------------------------------
 *
 * VMMouseUnplug  --
 * 	This function was called by Xserver when unplug
 *
 * Results:
 * 	None
 *
 * Side effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */

static void
VMMouseUnplug(pointer p)
{
   xf86Msg(X_INFO, "VMWARE(0): VMMouseUnplug\n");
}


/*
 *----------------------------------------------------------------------
 *
 * VMMousePlug  --
 * 	This function was called when Xserver load vmmouse module. It will
 * 	integrate the  module infto the XFree86 loader architecutre.
 *
 * Results:
 * 	true
 *
 * Side effects:
 * 	Regular mouse module was loaded as a submodule. In case
 * 	absolute pointing device is not available, we can always fall back
 *	to the regular mouse module
 *
 *----------------------------------------------------------------------
 */

static pointer
VMMousePlug(pointer	module,
	    pointer	options,
	    int		*errmaj,
	    int		*errmin)
{
   static bool Initialised = false;

   if (!Initialised)
      Initialised = true;

   xf86Msg(X_INFO, "VMWARE(0): VMMOUSE module was loaded\n");
   xf86AddInputDriver(&VMMOUSE, module, 0);

   return module;
}

static XF86ModuleVersionInfo VMMouseVersionRec = {
   "vmmouse",
   MODULEVENDORSTRING,
   MODINFOSTRING1,
   MODINFOSTRING2,
   XORG_VERSION_CURRENT,
   PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
   ABI_CLASS_XINPUT,
   ABI_XINPUT_VERSION,
   MOD_CLASS_XINPUT,
   {0, 0, 0, 0}		/* signature, to be patched into the file by a tool */
};

/*
 * The variable contains the necessary information to load and initialize the module
 */
_X_EXPORT XF86ModuleData vmmouseModuleData = {
   &VMMouseVersionRec,
   VMMousePlug,
   VMMouseUnplug
};
#endif /* XFree86LOADER */
