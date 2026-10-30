/*
 * ra_usb_probe.c — USB transport SPIKE (loader / PPC side).
 *
 * Enumerates the ESP32-S3 echo device on /dev/usb/ven and hands its USBV5
 * device id + bulk endpoints to the kernel, mirroring what the USB-storage
 * path does at 0x932C1000 (USBStorageOGC_Close): write the struct, flush,
 * then close the device and let the kernel keep using the device id.
 * Plan: c:/dev/wii/PLAN-USB-SPIKE.md.
 */

#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "usb_ogc.h"
#include "global.h"
#include "font.h"
#include "ra_usb_probe.h"
#include "../../common/include/RAUsbProbe.h"

/* The handoff + release serve both the spike and the production RA-over-USB
 * link (RA_USB_LINK); only the status drawing is spike-only. */
#if RA_USB_PROBE || RA_USB_LINK

/* ra_usbp_handoff_t.loader_err */
#define USBP_ERR_OPEN   -1   /* USB_OGC_OpenDevice failed */
#define USBP_ERR_DESC   -2   /* USB_OGC_GetDescriptors failed */
#define USBP_ERR_NOIF   -3   /* no vendor interface with bulk IN + OUT */

#define USBP_LIST_MAX   8
#define USBP_TRIES      20   /* x 100 ms: a fresh init fills the list async */

#define USB_CLASS_VENDOR_SPEC  0xFF
#define USB_XFER_BULK          0x02

void RA_USBP_Handoff(void)
{
	ra_usbp_handoff_t *h = (ra_usbp_handoff_t *)RA_USBP_HANDOFF_PPC;
	usb_device_entry list[USBP_LIST_MAX];
	usb_devdesc udd;
	usb_configurationdesc *ucd;
	usb_interfacedesc *uid;
	usb_endpointdesc *ued;
	u8 cnt, conf = 0, cfg_value = 0;
	s32 fd = -1, dev_id = 0;
	u32 c, f, e;
	int tries, i;
	bool found = false;

	/* Clear the handoff AND the kernel's result block (no stale numbers). */
	memset((void *)RA_USBP_HANDOFF_PPC, 0, RA_USBP_AREA_LEN);
	h->magic = RA_USBP_MAGIC_NOTFOUND;
	h->vid = RA_USBP_VID;
	h->pid = RA_USBP_PID;

	USB_OGC_Initialize();   /* no-op when the USB-storage probe already ran */

	for (tries = 0; tries < USBP_TRIES && !found; tries++)
	{
		cnt = 0;
		memset(list, 0, sizeof(list));
		if (USB_OGC_GetDeviceList(list, USBP_LIST_MAX, 0, &cnt) >= 0)
		{
			h->n_ven = cnt;
			for (i = 0; i < cnt; i++)
			{
				if (list[i].vid == RA_USBP_VID && list[i].pid == RA_USBP_PID)
				{
					dev_id = list[i].device_id;
					found = true;
					break;
				}
			}
		}
		if (!found)
			usleep(100000);
	}
	if (!found)
		goto out;

	if (USB_OGC_OpenDevice(dev_id, RA_USBP_VID, RA_USBP_PID, &fd) < 0)
	{
		h->loader_err = USBP_ERR_OPEN;
		goto out;
	}
	if (USB_OGC_GetDescriptors(fd, &udd) < 0)
	{
		h->loader_err = USBP_ERR_DESC;
		USB_OGC_CloseDevice(&fd);
		goto out;
	}

	for (c = 0; c < udd.bNumConfigurations && !h->ep_in; c++)
	{
		ucd = &udd.configurations[c];
		for (f = 0; f < ucd->bNumInterfaces && !h->ep_in; f++)
		{
			uid = &ucd->interfaces[f];
			if (uid->bInterfaceClass != USB_CLASS_VENDOR_SPEC || uid->bNumEndpoints < 2)
				continue;
			h->ep_in = h->ep_out = 0;
			for (e = 0; e < uid->bNumEndpoints; e++)
			{
				ued = &uid->endpoints[e];
				if ((ued->bmAttributes & 3) != USB_XFER_BULK)
					continue;
				if (ued->bEndpointAddress & USB_ENDPOINT_IN)
				{
					h->ep_in = ued->bEndpointAddress;
					h->mps_in = ued->wMaxPacketSize;
				}
				else
				{
					h->ep_out = ued->bEndpointAddress;
					h->mps_out = ued->wMaxPacketSize;
				}
			}
			if (h->ep_in && h->ep_out)
			{
				h->interface = uid->bInterfaceNumber;
				cfg_value = ucd->bConfigurationValue;
			}
			else
				h->ep_in = h->ep_out = 0;
		}
	}
	USB_OGC_FreeDescriptors(&udd);

	if (!h->ep_in)
	{
		h->loader_err = USBP_ERR_NOIF;
		USB_OGC_CloseDevice(&fd);
		goto out;
	}

	/* Same as the storage path: some devices error here, ignore it. */
	USB_OGC_GetConfiguration(fd, &conf);
	if (conf != cfg_value)
		USB_OGC_SetConfiguration(fd, cfg_value);

	h->device_id = fd;
	h->magic = RA_USBP_MAGIC_FOUND;

	/* The kernel keeps using the device id (USBV5 ids are global, not tied
	 * to our ven fd) — exactly like the USB-storage handoff. */
	USB_OGC_CloseDevice(&fd);

out:
	DCFlushRange((void *)RA_USBP_HANDOFF_PPC, RA_USBP_AREA_LEN);
}

void RA_USBP_Release(void)
{
	/* CloseDevices() only shuts USB down when a USB *drive* was mounted
	 * (disk_shutdown returns early otherwise). Booting from SD, the loader
	 * kept /dev/usb/ven open and the kernel's transfers all failed without
	 * a byte reaching the ESP (first HW run, 2026-10-12). Idempotent: a
	 * no-op when CloseDevices already did it. */
	USB_OGC_Deinitialize();
}

#if !RA_USB_PROBE

void RA_USBP_Draw(int y) { (void)y; }

#else

static void usbp_ms(char *buf, u32 us)
{
	sprintf(buf, "%u.%02u", us / 1000u, (us % 1000u) / 10u);
}

void RA_USBP_Draw(int y)
{
	const ra_usbp_handoff_t *h = (const ra_usbp_handoff_t *)RA_USBP_HANDOFF_PPC;
	const ra_usbp_result_t *r = (const ra_usbp_result_t *)RA_USBP_RESULT_PPC;
	char a[16], b[16], m[16], fl[16];

	DCInvalidateRange((void *)RA_USBP_HANDOFF_PPC, RA_USBP_AREA_LEN);

	if (h->magic == RA_USBP_MAGIC_NOTFOUND)
	{
		PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, y,
		            "USB probe: ESP %04X:%04X not found (ven=%u err=%d)",
		            h->vid, h->pid, h->n_ven, h->loader_err);
		return;
	}
	if (h->magic != RA_USBP_MAGIC_FOUND)
		return;

	PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, y,
	            "USB probe: ESP dev=%08X in=%02X out=%02X mps=%u",
	            (u32)h->device_id, h->ep_in, h->ep_out, h->mps_out);

	if (r->magic != RA_USBP_MAGIC_RESULT)
		return;
	if (r->stage == 1)
	{
		PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, y + 20,
		            "USB probe (idle): measuring row %u/6...", r->row);
		return;
	}
	if (r->ok_pairs == 0)
	{
		PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, y + 20,
		            "USB probe FAILED: ven_fd=%d first err kind=%d code=%d",
		            r->ven_fd, r->first_err_kind, r->first_err);
		PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, y + 40,
		            "errors %u   sd:/usbprobe.log %s", r->errors,
		            r->sdlog == 0 ? "written" : "NOT written");
		return;
	}
	usbp_ms(a, r->key_p50_us);
	usbp_ms(b, r->key_p90_us);
	usbp_ms(m, r->key_max_us);
	usbp_ms(fl, r->floor_p50_us);
	PrintFormat(DEFAULT_SIZE, r->errors ? MAROON : BLACK, MENU_POS_X, y + 20,
	            "idle 2048/8: p50 %s  p90 %s  max %s ms", a, b, m);
	PrintFormat(DEFAULT_SIZE, r->errors ? MAROON : BLACK, MENU_POS_X, y + 40,
	            "floor 64/8: p50 %s ms  err %u (kind %d code %d)", fl, r->errors,
	            r->first_err_kind, r->first_err);
}

#endif /* RA_USB_PROBE (draw) */

#else

void RA_USBP_Handoff(void) {}
void RA_USBP_Release(void) {}
void RA_USBP_Draw(int y) { (void)y; }

#endif /* RA_USB_PROBE || RA_USB_LINK */
