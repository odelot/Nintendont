/*
 * ra_usb_probe.h — USB transport SPIKE, loader side (see ra_usb_probe.c and
 * common/include/RAUsbProbe.h). Disposable; gated by RA_USB_PROBE.
 */

#ifndef __RA_USB_PROBE_LOADER_H__
#define __RA_USB_PROBE_LOADER_H__

/* Find the ESP echo device on /dev/usb/ven and write the kernel handoff.
 * Call right before CloseDevices() when launching a game (USB still up). */
void RA_USBP_Handoff(void);

/* Release /dev/usb/ven so the kernel can open it. Call right AFTER
 * CloseDevices() (which skips this when no USB drive was mounted). */
void RA_USBP_Release(void);

/* Draw the probe status (handoff + kernel idle-pass result) starting at
 * screen row y. Call from the loader's "waiting for kernel" loop. */
void RA_USBP_Draw(int y);

#endif /* __RA_USB_PROBE_LOADER_H__ */
