/*
 * ra_usb_link.h — RA console link over USB, Nintendont kernel side.
 *
 * The RetroAchievements protocol is unchanged; this only carries it over
 * bulk transfers to the adapter (ESP32-S3 VID:PID 303A:4052, firmware built
 * with CONFIG_RA_LINK_USB) instead of the memory-card slot. The loader finds
 * the adapter and hands its USBV5 device id over (RA_USBP_Handoff, see
 * common/include/RAUsbProbe.h); without a handoff the kernel stays on EXI.
 * Plan + measurements: c:/dev/wii/PLAN-USB-SPIKE.md §9-§12.
 */

#ifndef _RA_USB_LINK_H_
#define _RA_USB_LINK_H_

#include "global.h"

#define RA_USBL_ETIMEOUT  (-300)   /* no reply in time (exchange left in flight) */
#define RA_USBL_EBUSY     (-301)   /* the previous exchange is still in flight */
#define RA_USBL_EDEAD     (-302)   /* no link (no handoff / post failed) */
#define RA_USBL_EBUF      (-303)   /* buffer not DMA-safe and too big to bounce */

/* Largest request: an 8192-byte message would need a framing pad byte. */
#define RA_USBL_TX_MAX    8191u

/* Adopt the loader's handoff and open the shared /dev/usb/ven fd. 0 = the
 * adapter is on USB (use it for everything), < 0 = stay on EXI. */
s32 RA_UsbLink_Init(void);

/* One request/reply. Reply bytes [skip, skip+rx_len) land in rx: skip =
 * tx_len reproduces the EXI single-CS transaction (its write phase clocks out
 * the first tx_len reply bytes), skip = 0 the Phase-B read. A buffer the
 * transfer can't use directly is bounced; tx must have room for one more
 * byte when it is the (page-aligned) snapshot buffer. */
s32 RA_UsbLink_Xfer(const void *tx, u32 tx_len, void *rx, u32 rx_len, u32 skip, u32 timeout_ms);

#endif /* _RA_USB_LINK_H_ */
