/*
 * ra_usb_link.c — RA console link over USB, Nintendont kernel side.
 *
 * Twin of the d2x RAMOD usb_link.c (same rules, same wire), on top of this
 * kernel's USB layer (usb.c: the /dev/usb/ven fd shared with USB storage,
 * USB_BlkMsgAsync). The RA protocol is untouched — ra_module.c keeps its
 * EXI-shaped calls and they land here when the loader found the adapter.
 *
 * Wire rules (c:/dev/wii/PLAN-USB-SPIKE.md §9, §11):
 *   - every request gets exactly ONE reply, holding the bytes an EXI read
 *     would have clocked out (Phase-A 0xFF pads included), zero-padded by the
 *     ESP to a multiple of 4 (IN messages arrive cut down to one);
 *   - no message is a multiple of 64 bytes (a 0x00 byte is appended), so
 *     each one ends on a short packet;
 *   - one transfer crosses at most ONE 4 KB page boundary (OHCI; IOS does not
 *     split): dma_ok() checks it, small buffers are bounced;
 *   - async request blocks are separate 32-byte-aligned variables (an array
 *     of struct _usb_msg is misaligned past element 0 — spike round 5).
 *
 * The reply read is posted BEFORE the request (it must be waiting when the
 * ESP answers) and both complete on one queue; a timer token on the same
 * queue bounds the wait. On timeout the exchange stays in flight and the
 * next call finishes it first — the ESP answers every request, so the
 * request/reply alternation is never broken.
 */

#include "global.h"
#include "common.h"
#include "string.h"
#include "syscalls.h"
#include "usb.h"
#include "ra_usb_link.h"
#include "../common/include/RAUsbProbe.h"

#if RA_USB_LINK

#define TOK_XFER  0x7A000000u   /* never a kernel address (ipcmessage pointers) */

static struct _usb_msg   link_out_msg __attribute__((aligned(32)));
static struct _usb_msg   link_in_msg  __attribute__((aligned(32)));
static struct ipcmessage link_out_ipc __attribute__((aligned(32)));
static struct ipcmessage link_in_ipc  __attribute__((aligned(32)));
static u8  link_q_heap[0x80] __attribute__((aligned(32)));
static s32 link_q = -1;
static s32 link_dev = -1;
static u8  link_ep_in = 0x81;
static u8  link_ep_out = 0x01;
static bool link_up = false;
static bool link_dead = false;
static u32 link_tok = 0;

/* Bounces: requests built on the stack (debug lines <= 4+1+64), and short
 * replies whose head must be skipped (default_ack is 14 B -> 16 on the wire). */
static u8 small_out[288] __attribute__((aligned(32)));
static u8 small_in[128]  __attribute__((aligned(32)));

static struct {
	bool busy;
	bool out_done;
	bool in_done;
	s32  out_res;
	s32  in_res;
	u32  out_len;
	u8  *in_buf;
	u32  in_cap;
} xs;

/* 32-byte aligned and at most one 4 KB page crossing. */
static int dma_ok(const void *p, u32 len)
{
	u32 a = (u32)p;
	if (a & 31u)
		return 0;
	return ((a & 4095u) + len) <= 8192u;
}

s32 RA_UsbLink_Init(void)
{
	const ra_usbp_handoff_t *h = (const ra_usbp_handoff_t *)RA_USBP_HANDOFF_PHYS;

	if (link_up)
		return 0;
	sync_before_read((void *)RA_USBP_HANDOFF_PHYS, sizeof(ra_usbp_handoff_t));
	if (h->magic != RA_USBP_MAGIC_FOUND)
		return RA_USBL_EDEAD;   /* loader saw no USB adapter: EXI it is */
	link_dev = h->device_id;
	if (h->ep_in)  link_ep_in = h->ep_in;
	if (h->ep_out) link_ep_out = h->ep_out;

	USB_Initialize();          /* no-op when USB storage already opened ven */
	if (USB_GetVenFd() < 0)
		return RA_USBL_EDEAD;
	if (link_q < 0)
		link_q = mqueue_create(link_q_heap, 8);
	if (link_q < 0)
		return RA_USBL_EDEAD;
	link_up = true;
	return 0;
}

static s32 xs_result(void)
{
	if (xs.out_res != (s32)xs.out_len)
		return xs.out_res < 0 ? xs.out_res : RA_USBL_EDEAD;
	if (xs.in_res < 0)
		return xs.in_res;
	return 0;
}

static s32 xs_wait(u32 timeout_ms)
{
	struct ipcmessage *msg;
	u32 tok = 0;
	s32 t = -1;

	if (!xs.busy)
		return 0;
	if (timeout_ms) {
		tok = TOK_XFER | (++link_tok & 0xFFFFu);
		t = TimerCreate((int)(timeout_ms * 1000u), 0, link_q, (int)tok);
	}
	while (!(xs.out_done && xs.in_done)) {
		msg = NULL;
		mqueue_recv(link_q, &msg, 0);
		if (msg == &link_out_ipc) {
			xs.out_res = (s32)msg->result;
			mqueue_ack(msg, 0);
			xs.out_done = true;
		} else if (msg == &link_in_ipc) {
			xs.in_res = (s32)msg->result;
			mqueue_ack(msg, 0);
			xs.in_done = true;
		} else if (t >= 0 && (u32)msg == tok) {
			break;
		}
		/* anything else: an earlier wait's timer that fired late */
	}
	if (t >= 0)
		TimerDestroy(t);
	if (!(xs.out_done && xs.in_done))
		return RA_USBL_ETIMEOUT;
	xs.busy = false;
	if (xs.in_res > 0)
		sync_before_read(xs.in_buf, xs.in_cap);   /* DMA'd reply bytes */
	return 0;
}

static s32 xs_post(const void *out, u32 out_len, void *in, u32 in_cap)
{
	s32 r;

	if (link_dead)
		return RA_USBL_EDEAD;
	if (xs.busy)
		return RA_USBL_EBUSY;
	xs.out_done = false;
	xs.in_done = false;
	xs.out_res = 0;
	xs.in_res = 0;
	xs.out_len = out_len;
	xs.in_buf = (u8 *)in;
	xs.in_cap = in_cap;

	sync_after_write((void *)out, out_len);
	sync_after_write(in, in_cap);   /* no dirty line may land on the reply later */

	r = USB_BlkMsgAsync(link_dev, link_ep_in, (u16)in_cap, in,
	                    &link_in_msg, link_q, &link_in_ipc);
	if (r < 0)
		return r;
	xs.busy = true;
	r = USB_BlkMsgAsync(link_dev, link_ep_out, (u16)out_len, (void *)out,
	                    &link_out_msg, link_q, &link_out_ipc);
	if (r < 0) {
		/* The read stays posted with no request behind it: the link is
		 * unusable until the adapter re-enumerates. */
		xs.out_done = true;
		xs.out_res = r;
		link_dead = true;
		return RA_USBL_EDEAD;
	}
	return 0;
}

s32 RA_UsbLink_Xfer(const void *tx, u32 tx_len, void *rx, u32 rx_len, u32 skip, u32 timeout_ms)
{
	u8 *out, *in;
	u32 n, cap, c;
	s32 r;

	if (!link_up)
		return RA_USBL_EDEAD;
	/* A timed-out exchange finishes first (its late reply is dropped). */
	if (xs_wait(timeout_ms) < 0)
		return RA_USBL_EBUSY;

	if (tx_len + 1 <= sizeof(small_out)) {
		memcpy(small_out, tx, tx_len);
		out = small_out;
	} else if (tx_len <= RA_USBL_TX_MAX && dma_ok(tx, tx_len + 1)) {
		out = (u8 *)tx;   /* the snapshot buffer: room for the pad byte */
	} else {
		return RA_USBL_EBUF;
	}
	n = tx_len;
	if ((n & 63u) == 0)
		out[n++] = 0;

	if (rx && skip == 0 && rx_len > sizeof(small_in) && dma_ok(rx, rx_len)) {
		in = (u8 *)rx;
		cap = rx_len;
	} else if (skip + rx_len <= sizeof(small_in)) {
		in = small_in;
		cap = sizeof(small_in);
	} else {
		return RA_USBL_EBUF;
	}

	r = xs_post(out, n, in, cap);
	if (r < 0)
		return r;
	r = xs_wait(timeout_ms);
	if (r < 0)
		return r;
	r = xs_result();
	if (r < 0)
		return r;
	if (in != (u8 *)rx && rx && rx_len) {
		memset(rx, 0, rx_len);
		if ((u32)xs.in_res > skip) {
			c = (u32)xs.in_res - skip;
			if (c > rx_len)
				c = rx_len;
			memcpy(rx, small_in + skip, c);
		}
	}
	return 0;
}

#else /* !RA_USB_LINK */

s32 RA_UsbLink_Init(void) { return RA_USBL_EDEAD; }
s32 RA_UsbLink_Xfer(const void *tx, u32 tx_len, void *rx, u32 rx_len, u32 skip, u32 timeout_ms)
{
	(void)tx; (void)tx_len; (void)rx; (void)rx_len; (void)skip; (void)timeout_ms;
	return RA_USBL_EDEAD;
}

#endif /* RA_USB_LINK */
