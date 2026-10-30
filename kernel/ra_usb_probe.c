/*
 * ra_usb_probe.c — USB transport SPIKE (Nintendont kernel side).
 *
 * Times transfers over USB bulk (/dev/usb/ven) against the ESP32-S3 echo
 * firmware (c:/dev/wii/usb-spike-esp). Plan and decision tree:
 * c:/dev/wii/PLAN-USB-SPIKE.md.
 *
 * Disposable and isolated: reuses the kernel's existing USBV5 client
 * (usb.c, the same ven_fd the USB-storage path uses) and never touches the
 * EXI production path. Everything is compiled out with RA_USB_PROBE 0.
 *
 * What the hardware rounds established (2026-10-12):
 *   - The OHCI reports a transfer done only at a USB frame boundary, so every
 *     ioctl costs a whole frame: ~1 ms idle, ~2 ms with a game running
 *     (thread priority ruled out: 0x78 and 0x50 measure identical).
 *   - Bandwidth is ~850 B/ms. A strict request/response ping-pong is the
 *     worst case for that: 2048/8 = 5.7 ms in game, of which the 8-byte
 *     reply alone is ~2 ms. Dropping the reply ("noreply") gives 3.8 ms.
 *   - An IN posted alongside the OUT ("async") gains nothing: the OUT then
 *     completes together with the IN.
 *
 * This round measures the two production shapes that avoid the ping-pong:
 *   standing  one IN stays posted; the ESP completes it only when it has
 *             something to say (here: every 32nd request). Question: does a
 *             pending, NAKed IN slow the per-frame OUT down?
 *   stream    two buffers in flight, OUTs posted asynchronously; the thread
 *             only waits when both are still on the wire (VR-headset style).
 *             Question: is 60 Hz x 5 KB (Wii) sustainable with the thread
 *             barely blocking? Back-to-back rows give the max throughput.
 * plus, when the game runs from a USB drive, a high-speed (EHCI) latency
 * proxy: timed sector reads through the kernel's USB-storage driver.
 *
 * Reports: the idle pass summary on the loader screen (result block at
 * RA_USBP_RESULT_PHYS), the idle pass in sd:/usbprobe.log, and every row /
 * soak window as a LOG message the ESP prints on its UART console.
 */

#include "global.h"
#include "common.h"
#include "string.h"
#include "syscalls.h"
#include "Config.h"
#include "usb.h"
#include "usbstorage.h"
#include "vsprintf.h"
#include "ff_utf8.h"
#include "ra_usb_probe.h"
#include "../common/include/RAUsbProbe.h"

#if RA_USB_PROBE

extern u32 s_size;   /* boot device sector size (diskio.c) */

/* Largest request. The Wii's full-speed controller (OHCI) can cross only ONE
 * 4 KB page boundary per transfer descriptor and IOS does not split: a
 * transfer spanning 3 pages silently loses the middle page while the ioctl
 * still reports full length (first HW run: 8193 B from 0x...3080 arrived as
 * 3968 + 129 = 4097 B). So buffers are page-aligned and no transfer may
 * exceed 8192 bytes — a hard rule for any production USB transport. */
#define USBP_TX_MAX     8192
/* Read buffer, deliberately larger than any reply: the reply's short packet
 * ends the transfer, as a production reply of unknown length would. */
#define USBP_RX_MAX     2560
/* Soak window capacity (60 Hz x RA_USBP_WINDOW_SECS, plus slack). */
#define USBP_SAMPLES    ((RA_USBP_ITERS > 320) ? RA_USBP_ITERS : 320)
/* Standing mode: the ESP answers every Nth request. */
#define USBP_STAND_EVERY 32

/* 60 Hz frame in HW_TIMER ticks (1 tick ~ 526.7 ns), as in ra_module.c. */
#define USBP_FRAME_TICKS  ((16667u * 1000u) / 527u)

enum { USBP_SYNC = 0, USBP_NOREPLY };

typedef struct
{
	u16 out_len;
	u16 resp_len;
	const char *name;
} usbp_row_t;

static const usbp_row_t usbp_rows[] =
{
	{   64,    8, "floor"      },
	{ 2048,    8, "gc"         },
	{ 4096,    8, "gc-worst"   },
	{ 5120,    8, "wii"        },
	{ 8191,    8, "wii-worst"  },   /* 8192 would be padded to 8193 = 3 pages */
	{ 2048, 2080, "exi-parity" },
};
#define USBP_NROWS     (sizeof(usbp_rows) / sizeof(usbp_rows[0]))
#define USBP_ROW_FLOOR 0
#define USBP_ROW_GC    1
#define USBP_ROW_WII   3
#define USBP_ROW_WIIW  4

typedef struct
{
	u32 n_ok, n_err, n_bad;
	u32 p50, p90, p99, max;     /* s_pair: full exchange, us */
	u32 out50, out90;           /* s_out */
	u32 in50, in90;             /* s_in */
} usbp_stats_t;

static u8 usbp_tx[USBP_TX_MAX] __attribute__((aligned(4096)));
static u8 usbp_rx[USBP_RX_MAX] __attribute__((aligned(32)));
static u32 s_pair[USBP_SAMPLES], s_out[USBP_SAMPLES], s_in[USBP_SAMPLES];
static u32 s_sort[USBP_SAMPLES];
static char usbp_line[320];   /* longest line ~230 chars; _vsprintf is unbounded */

/* Standing IN: request block + completion message + 1-slot queue. */
static struct _usb_msg usbp_in_msg __attribute__((aligned(32)));
static struct ipcmessage usbp_in_ipc __attribute__((aligned(32)));
static u8 usbp_qin_heap[0x20] __attribute__((aligned(32)));
static s32 usbp_qin = -1;

/* Stream: two OUT buffers in flight, 2-slot queue. Buffer 0 is usbp_tx;
 * buffer 1 comes from the heap (page-aligned 8 KB) or, if that fails, both
 * are the two 4 KB halves of usbp_tx (then rows above 4095 B are skipped). */
/* Separate variables, NOT arrays: sizeof(struct _usb_msg) is 88, so in an
 * array only element 0 is 32-aligned and USB_BlkMsgAsync rejects element 1
 * (round 5 build: every post from buffer 1 failed with IPC_EINVAL). */
static struct _usb_msg usbp_out_msg0 __attribute__((aligned(32)));
static struct _usb_msg usbp_out_msg1 __attribute__((aligned(32)));
static struct ipcmessage usbp_out_ipc0 __attribute__((aligned(32)));
static struct ipcmessage usbp_out_ipc1 __attribute__((aligned(32)));
static struct _usb_msg *const usbp_out_msg[2] = { &usbp_out_msg0, &usbp_out_msg1 };
static struct ipcmessage *const usbp_out_ipc[2] = { &usbp_out_ipc0, &usbp_out_ipc1 };
static u8 usbp_qout_heap[0x40] __attribute__((aligned(32)));
static s32 usbp_qout = -1;
static u8 *usbp_sbuf[2];
static u32 usbp_scap;                 /* max request per stream buffer */
static u32 usbp_s_phys[2], usbp_s_t[2], usbp_s_inflight, usbp_s_next;

static bool usbp_ready = false;
static s32 usbp_dev = -1;
static u8 usbp_ep_in, usbp_ep_out;
static u32 usbp_seq;
static s32 usbp_last_ret;     /* raw return of the last failing ioctl */
static s32 usbp_first_err, usbp_first_kind;   /* first failing exchange */

/* /usbprobe.log on the boot device. Open ONLY during the idle pass: FatFs is
 * not reentrant (_FS_REENTRANT 0), and once the game runs the DI thread owns
 * it. FIL is >4 KB (_MAX_SS 4096), so it lives on the heap, not in .bss. */
static FIL *usbp_fil;
static s32 usbp_sdlog = -1;

/* Session totals (all passes), reported with every soak window. */
static u32 tot_pairs, tot_err, tot_bad, tot_worst;

static u32 usbp_thread_id;

static u32 usbp_us(u32 ticks)
{
	/* HW_TIMER runs at 243 MHz / 128 = 1.8984375 MHz. */
	return (ticks / 1898u) * 1000u + ((ticks % 1898u) * 1000u) / 1898u;
}

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void usbp_publish(s32 stage, u32 row, const usbp_stats_t *key,
                         u32 floor_p50, u32 errors)
{
	ra_usbp_result_t *r = (ra_usbp_result_t *)RA_USBP_RESULT_PHYS;
	r->magic = RA_USBP_MAGIC_RESULT;
	r->stage = stage;
	r->row = row;
	if (key)
	{
		r->key_p50_us = key->p50;
		r->key_p90_us = key->p90;
		r->key_max_us = key->max;
	}
	if (floor_p50)
		r->floor_p50_us = floor_p50;
	r->errors = errors;
	r->ven_fd = USB_GetVenFd();
	r->first_err = usbp_first_err;
	r->first_err_kind = usbp_first_kind;
	r->ok_pairs = tot_pairs;
	r->sdlog = usbp_sdlog;
	sync_after_write(r, sizeof(*r));
}

/* Write one request into tx; returns its padded (wire) length. */
static u32 usbp_build(u8 *tx, u8 cmd, u8 flags, u32 out_len, u32 resp_len,
                      const char *text, u32 seq)
{
	u32 i, phys = RA_USBP_PADDED(out_len);

	tx[0] = RA_USBP_REQ_MAGIC;
	tx[1] = cmd;
	tx[2] = (u8)(out_len >> 8);
	tx[3] = (u8)out_len;
	tx[4] = (u8)(resp_len >> 8);
	tx[5] = (u8)resp_len;
	tx[6] = flags;
	tx[7] = 0;
	tx[8] = (u8)(seq >> 24);
	tx[9] = (u8)(seq >> 16);
	tx[10] = (u8)(seq >> 8);
	tx[11] = (u8)seq;
	if (cmd == RA_USBP_CMD_LOG)
		memcpy(tx + RA_USBP_HDR_LEN, text, out_len - RA_USBP_HDR_LEN);
	else
		for (i = RA_USBP_HDR_LEN; i < out_len; i++)
			tx[i] = (u8)(seq + i);
	if (phys != out_len)
		tx[out_len] = 0;
	return phys;
}

/* Validate a reply of r bytes in usbp_rx. 0 = ok, 1 = malformed/corrupt,
 * 2 = the ESP flagged the request (raw code in usbp_last_ret). */
static s32 usbp_check_reply(s32 r, u32 seq, u32 resp_len, u32 phys)
{
	const u8 *rx = usbp_rx;
	u32 i;

	if (r != (s32)RA_USBP_PADDED(resp_len) || rx[0] != RA_USBP_RESP_MAGIC ||
	    be32(rx + 4) != seq)
	{
		usbp_last_ret = r;
		return 1;
	}
	if (rx[1] != RA_USBP_ST_OK)
	{
		usbp_last_ret = rx[1];
		return 2;
	}
	if ((((u32)rx[2] << 8) | rx[3]) != phys)
	{
		usbp_last_ret = ((u32)rx[2] << 8) | rx[3];
		return 1;
	}
	for (i = RA_USBP_RESP_HDR_LEN; i < resp_len; i++)
	{
		if (rx[i] != (u8)((seq ^ 0xA5u) + i))
		{
			usbp_last_ret = (s32)i;
			return 1;
		}
	}
	return 0;
}

/* One blocking exchange. Returns 0 = ok, -1 = OUT failed, -2 = IN failed
 * (raw code in usbp_last_ret), >0 = usbp_check_reply verdict. *us_out /
 * *us_in only valid on 0. In noreply mode the ESP's verdict is only
 * visible in its own counters. */
static s32 usbp_exchange(u8 cmd, u32 mode, u32 out_len, u32 resp_len,
                         const char *text, u32 *us_out, u32 *us_in)
{
	u32 seq, phys, t0, t1, t2;
	s32 r;

	seq = ++usbp_seq;
	phys = usbp_build(usbp_tx, cmd, mode == USBP_NOREPLY ? RA_USBP_FLAG_NO_REPLY : 0,
	                  out_len, resp_len, text, seq);

	/* Timed region = what a production transport would pay per snapshot:
	 * cache maintenance + the ioctls. Payload building stays outside. */
	t0 = read32(HW_TIMER);
	sync_after_write(usbp_tx, (phys + 31u) & ~31u);
	r = USB_WriteBlkMsg(usbp_dev, usbp_ep_out, (u16)phys, usbp_tx);
	t1 = read32(HW_TIMER);
	if (r != (s32)phys)
	{
		usbp_last_ret = r;
		return -1;
	}
	if (mode == USBP_NOREPLY)
	{
		*us_out = usbp_us(t1 - t0);
		*us_in = 0;
		return 0;
	}
	r = USB_ReadBlkMsg(usbp_dev, usbp_ep_in, USBP_RX_MAX, usbp_rx);
	sync_before_read(usbp_rx, USBP_RX_MAX);
	t2 = read32(HW_TIMER);
	if (r < 0)
	{
		usbp_last_ret = r;
		return -2;
	}
	r = usbp_check_reply(r, seq, resp_len, phys);
	if (r)
		return r;

	*us_out = usbp_us(t1 - t0);
	*us_in = usbp_us(t2 - t1);
	return 0;
}

/* Ship one formatted line to the ESP console (and to /usbprobe.log while
 * the idle pass has it open). Always a blocking sync exchange, so callers
 * must not have a standing IN or stream OUT in flight. Best effort. */
static void usbp_log(const char *fmt, ...)
{
	va_list args;
	u32 n, uo, ui;
	UINT wrote;

	va_start(args, fmt);
	_vsprintf(usbp_line, fmt, args);
	va_end(args);
	n = strlen(usbp_line);

	if (usbp_fil)
	{
		f_write(usbp_fil, usbp_line, n, &wrote);
		f_write(usbp_fil, "\r\n", 2, &wrote);
	}
	if (usbp_ready)
		usbp_exchange(RA_USBP_CMD_LOG, USBP_SYNC, RA_USBP_HDR_LEN + n,
		              RA_USBP_RESP_HDR_LEN, usbp_line, &uo, &ui);
}

/* Sorted copy of the first n entries of src into s_sort. */
static void usbp_sort(const u32 *src, u32 n)
{
	u32 i, j, v;
	for (i = 0; i < n; i++)
	{
		v = src[i];
		for (j = i; j > 0 && s_sort[j - 1] > v; j--)
			s_sort[j] = s_sort[j - 1];
		s_sort[j] = v;
	}
}

static u32 usbp_pct(u32 n, u32 pct)
{
	return n ? s_sort[((n - 1) * pct) / 100u] : 0;
}

/* Fill st's percentiles from s_out[0..n_out), s_in[0..n_in), s_pair[0..n_pair). */
static void usbp_summarize3(u32 n_out, u32 n_in, u32 n_pair, usbp_stats_t *st)
{
	usbp_sort(s_out, n_out);
	st->out50 = usbp_pct(n_out, 50);
	st->out90 = usbp_pct(n_out, 90);
	usbp_sort(s_in, n_in);
	st->in50 = usbp_pct(n_in, 50);
	st->in90 = usbp_pct(n_in, 90);
	usbp_sort(s_pair, n_pair);
	st->p50 = usbp_pct(n_pair, 50);
	st->p90 = usbp_pct(n_pair, 90);
	st->p99 = usbp_pct(n_pair, 99);
	st->max = n_pair ? s_sort[n_pair - 1] : 0;
}

static void usbp_summarize(u32 n, usbp_stats_t *st)
{
	usbp_summarize3(n, n, n, st);
}

static void usbp_account(const usbp_stats_t *st)
{
	tot_pairs += st->n_ok;
	tot_err += st->n_err;
	tot_bad += st->n_bad;
	if (st->max > tot_worst)
		tot_worst = st->max;
}

static void usbp_note_failure(s32 r)
{
	if (r != 0 && usbp_first_kind == 0)
	{
		usbp_first_kind = r;
		usbp_first_err = usbp_last_ret;
	}
}

/* ---- sync / noreply rows ------------------------------------------------- */

static void usbp_run_row(const usbp_row_t *row, u32 mode, usbp_stats_t *st)
{
	u32 k, n = 0, consec = 0, uo, ui;
	s32 r;

	memset(st, 0, sizeof(*st));
	for (k = 0; k < RA_USBP_ITERS; k++)
	{
		r = usbp_exchange(RA_USBP_CMD_ECHO, mode, row->out_len, row->resp_len,
		                  NULL, &uo, &ui);
		usbp_note_failure(r);
		if (r < 0)
		{
			st->n_err++;
			if (++consec >= 10)
				break;   /* device gone or wedged: stop hammering */
			continue;
		}
		consec = 0;
		if (r > 0)
		{
			st->n_bad++;
			continue;
		}
		s_out[n] = uo;
		s_in[n] = ui;
		s_pair[n] = uo + ui;
		n++;
	}
	st->n_ok = n;
	usbp_summarize(n, st);
	usbp_account(st);
}

static void usbp_report_row(const char *label, u32 pass, const usbp_row_t *row,
                            u32 mode, const usbp_stats_t *st)
{
	u32 thr = st->p50 ? ((u32)(row->out_len + row->resp_len) * 1000u) / st->p50 : 0;
	usbp_log("USBP %s#%u %s %s %u/%u ok=%u err=%u bad=%u last=%d "
	         "pair50/90/99/max=%u/%u/%u/%u out50/90=%u/%u in50/90=%u/%u us thr=%uB/ms",
	         label, pass, mode == USBP_NOREPLY ? "noreply" : "sync", row->name,
	         row->out_len, row->resp_len, st->n_ok, st->n_err, st->n_bad,
	         st->n_err + st->n_bad ? usbp_last_ret : 0,
	         st->p50, st->p90, st->p99, st->max,
	         st->out50, st->out90, st->in50, st->in90, thr);
}

/* ---- standing IN --------------------------------------------------------- */

static s32 usbp_standing_post(void)
{
	return USB_BlkMsgAsync(usbp_dev, usbp_ep_in, USBP_RX_MAX, usbp_rx,
	                       &usbp_in_msg, usbp_qin, &usbp_in_ipc);
}

/* One standing-mode frame: an OUT while the standing IN waits. When
 * want_reply, the ESP answers it, completing the standing IN, which is then
 * validated and re-posted (if repost). *us_out = the OUT ioctl; *us_reply =
 * request start to reply validated (0 when no reply). */
static s32 usbp_standing_xfer(u32 out_len, bool want_reply, bool repost,
                              u32 *us_out, u32 *us_reply)
{
	struct ipcmessage *done;
	u32 seq, phys, t0, t1, t2;
	s32 r, v;

	seq = ++usbp_seq;
	phys = usbp_build(usbp_tx, RA_USBP_CMD_ECHO, want_reply ? 0 : RA_USBP_FLAG_NO_REPLY,
	                  out_len, RA_USBP_RESP_HDR_LEN, NULL, seq);
	t0 = read32(HW_TIMER);
	sync_after_write(usbp_tx, (phys + 31u) & ~31u);
	r = USB_WriteBlkMsg(usbp_dev, usbp_ep_out, (u16)phys, usbp_tx);
	t1 = read32(HW_TIMER);
	if (r != (s32)phys)
	{
		usbp_last_ret = r;
		return -1;
	}
	*us_out = usbp_us(t1 - t0);
	*us_reply = 0;
	if (!want_reply)
		return 0;

	mqueue_recv(usbp_qin, &done, 0);
	r = (s32)done->result;
	mqueue_ack(done, 0);
	sync_before_read(usbp_rx, USBP_RX_MAX);
	t2 = read32(HW_TIMER);
	v = (r < 0) ? -2 : usbp_check_reply(r, seq, RA_USBP_RESP_HDR_LEN, phys);
	if (r < 0)
		usbp_last_ret = r;
	if (repost && usbp_standing_post() < 0)
		return -2;
	*us_reply = usbp_us(t2 - t0);
	return v;
}

/* RA_USBP_ITERS standing frames. s_out = OUT of the no-reply frames (the
 * per-frame cost), s_pair = reply frames (request -> reply validated). */
static void usbp_run_standing(const usbp_row_t *row, usbp_stats_t *st)
{
	u32 k, n_out = 0, n_rep = 0, uo, ur;
	bool want;
	s32 r;

	memset(st, 0, sizeof(*st));
	if (usbp_standing_post() < 0)
	{
		st->n_err = 1;
		return;
	}
	for (k = 0; k < RA_USBP_ITERS; k++)
	{
		want = (k % USBP_STAND_EVERY) == USBP_STAND_EVERY - 1;
		r = usbp_standing_xfer(row->out_len, want, true, &uo, &ur);
		usbp_note_failure(r);
		if (r < 0) { st->n_err++; continue; }
		if (r > 0) { st->n_bad++; continue; }
		if (want)
			s_pair[n_rep++] = ur;
		else
			s_out[n_out++] = uo;
	}
	/* Reap the standing IN with one last answered request (not counted). */
	r = usbp_standing_xfer(64, true, false, &uo, &ur);
	usbp_note_failure(r);
	if (r != 0)
		st->n_err++;

	st->n_ok = n_out + n_rep;
	usbp_summarize3(n_out, 0, n_rep, st);
	usbp_account(st);
}

static void usbp_report_standing(const char *label, u32 pass, const usbp_row_t *row,
                                 const usbp_stats_t *st)
{
	usbp_log("USBP %s#%u standing %s %u ok=%u err=%u bad=%u last=%d "
	         "out50/90 (IN pending)=%u/%u us reply-frames n=%u p50/max=%u/%u us",
	         label, pass, row->name, row->out_len, st->n_ok, st->n_err, st->n_bad,
	         st->n_err + st->n_bad ? usbp_last_ret : 0,
	         st->out50, st->out90, RA_USBP_ITERS / USBP_STAND_EVERY, st->p50, st->max);
}

/* ---- stream (two OUTs in flight) ---------------------------------------- */

/* Wait for the oldest in-flight OUT (the queue delivers in completion
 * order, and one endpoint completes in submission order). 0 = sent in
 * full; *lat_us = post -> reaped. */
static s32 usbp_stream_reap(u32 *lat_us)
{
	struct ipcmessage *done;
	u32 k;
	s32 r;

	mqueue_recv(usbp_qout, &done, 0);
	k = (done == usbp_out_ipc[0]) ? 0 : 1;
	r = (s32)done->result;
	mqueue_ack(done, 0);
	*lat_us = usbp_us(read32(HW_TIMER) - usbp_s_t[k]);
	usbp_s_inflight &= ~(1u << k);
	if (r != (s32)usbp_s_phys[k])
	{
		usbp_last_ret = r;
		return -1;
	}
	return 0;
}

/* Post one request from the next buffer, waiting only if that buffer is
 * still on the wire. *block_us = time the thread spent on USB for this post
 * (wait + submit), payload building excluded. */
static s32 usbp_stream_post(u32 out_len, u32 *block_us, u32 *lat_us, bool *reaped)
{
	u32 k = usbp_s_next, t0, t1, t2, t3;
	s32 r = 0, p;

	*reaped = false;
	*block_us = 0;
	t0 = read32(HW_TIMER);
	if (usbp_s_inflight & (1u << k))
	{
		r = usbp_stream_reap(lat_us);
		*reaped = true;
	}
	t1 = read32(HW_TIMER);

	usbp_s_phys[k] = usbp_build(usbp_sbuf[k], RA_USBP_CMD_ECHO, RA_USBP_FLAG_NO_REPLY,
	                            out_len, RA_USBP_RESP_HDR_LEN, NULL, ++usbp_seq);

	t2 = read32(HW_TIMER);
	sync_after_write(usbp_sbuf[k], (usbp_s_phys[k] + 31u) & ~31u);
	usbp_s_t[k] = t2;
	p = USB_BlkMsgAsync(usbp_dev, usbp_ep_out, (u16)usbp_s_phys[k], usbp_sbuf[k],
	                    usbp_out_msg[k], usbp_qout, usbp_out_ipc[k]);
	if (p < 0)
	{
		usbp_last_ret = p;   /* the real IOS/EINVAL code, not a sentinel */
		return -1;
	}
	usbp_s_inflight |= 1u << k;
	usbp_s_next ^= 1;
	t3 = read32(HW_TIMER);
	*block_us = usbp_us(t1 - t0) + usbp_us(t3 - t2);
	return r;
}

static u32 usbp_stream_drain(u32 *n_lat)
{
	u32 lat, errs = 0;
	while (usbp_s_inflight)
	{
		if (usbp_stream_reap(&lat) < 0)
			errs++;
		else if (*n_lat < USBP_SAMPLES)
			s_in[(*n_lat)++] = lat;
	}
	return errs;
}

/* Back-to-back stream: RA_USBP_ITERS posts as fast as the bus takes them.
 * s_out = per-post block time, s_in = completion latency, throughput from
 * the wall time of the whole row. */
static void usbp_run_stream(const usbp_row_t *row, usbp_stats_t *st, u32 *thr_bpms)
{
	u32 k, n_blk = 0, n_lat = 0, blk, lat, t0, el;
	bool reaped;
	s32 r;

	memset(st, 0, sizeof(*st));
	*thr_bpms = 0;
	t0 = read32(HW_TIMER);
	for (k = 0; k < RA_USBP_ITERS; k++)
	{
		r = usbp_stream_post(row->out_len, &blk, &lat, &reaped);
		usbp_note_failure(r);
		if (r < 0)
			st->n_err++;
		else if (reaped && n_lat < USBP_SAMPLES)
			s_in[n_lat++] = lat;
		s_out[n_blk++] = blk;
	}
	st->n_err += usbp_stream_drain(&n_lat);
	el = usbp_us(read32(HW_TIMER) - t0);
	if (el)   /* 200 x 8191 x 1000 = 1.64e9: fits in u32 */
		*thr_bpms = (RA_USBP_ITERS * (u32)row->out_len * 1000u) / el;
	st->n_ok = RA_USBP_ITERS - st->n_err;
	usbp_summarize3(n_blk, n_lat, 0, st);
	usbp_account(st);
}

static void usbp_report_stream(const char *label, u32 pass, const usbp_row_t *row,
                               const usbp_stats_t *st, u32 thr)
{
	usbp_log("USBP %s#%u stream %s %u ok=%u err=%u last=%d block50/90=%u/%u us "
	         "complete50/90=%u/%u us thr=%uB/ms (%u per 60Hz frame)",
	         label, pass, row->name, row->out_len, st->n_ok, st->n_err,
	         st->n_err ? usbp_last_ret : 0, st->out50, st->out90,
	         st->in50, st->in90, thr, (thr * 16667u) / 1000u);
}

/* ---- matrix + soak -------------------------------------------------------- */

static bool usbp_stream_fits(const usbp_row_t *row)
{
	return RA_USBP_PADDED(row->out_len) <= usbp_scap;
}

/* Sync matrix (baseline), noreply key rows, standing and stream rows. On
 * the idle pass the sync rows also feed the loader screen. */
static void usbp_matrix(const char *label, u32 pass, bool publish)
{
	static const u8 noreply_rows[] = { USBP_ROW_FLOOR, USBP_ROW_GC, USBP_ROW_WII };
	static const u8 standing_rows[] = { USBP_ROW_GC, USBP_ROW_WII };
	static const u8 stream_rows[] = { USBP_ROW_GC, USBP_ROW_WII, USBP_ROW_WIIW };
	u32 i, errors = 0, floor_p50 = 0, thr;
	usbp_stats_t st, key;

	memset(&key, 0, sizeof(key));
	for (i = 0; i < USBP_NROWS; i++)
	{
		if (publish)
			usbp_publish(1, i + 1, NULL, 0, errors);
		usbp_run_row(&usbp_rows[i], USBP_SYNC, &st);
		errors += st.n_err + st.n_bad;
		if (i == USBP_ROW_FLOOR)
			floor_p50 = st.p50;
		if (i == USBP_ROW_GC)
			key = st;
		usbp_report_row(label, pass, &usbp_rows[i], USBP_SYNC, &st);
	}
	if (publish)
		usbp_publish(2, USBP_NROWS, &key, floor_p50, errors);

	for (i = 0; i < sizeof(noreply_rows); i++)
	{
		usbp_run_row(&usbp_rows[noreply_rows[i]], USBP_NOREPLY, &st);
		usbp_report_row(label, pass, &usbp_rows[noreply_rows[i]], USBP_NOREPLY, &st);
	}
	for (i = 0; i < sizeof(standing_rows); i++)
	{
		usbp_run_standing(&usbp_rows[standing_rows[i]], &st);
		usbp_report_standing(label, pass, &usbp_rows[standing_rows[i]], &st);
	}
	for (i = 0; i < sizeof(stream_rows); i++)
	{
		const usbp_row_t *row = &usbp_rows[stream_rows[i]];
		if (!usbp_stream_fits(row))
		{
			usbp_log("USBP %s#%u stream %s skipped (buffers are %u B)",
			         label, pass, row->name, usbp_scap);
			continue;
		}
		usbp_run_stream(row, &st, &thr);
		usbp_report_stream(label, pass, row, &st, thr);
	}
}

/* Soak flavours, one per 5 s window, rotating: the production shapes at
 * 60 Hz on the same scene. 12 windows = each flavour twice per pass. */
enum { SOAK_SYNC, SOAK_NOREPLY, SOAK_STANDING, SOAK_STREAM };
static const struct { u8 kind; u8 row; const char *name; } usbp_soak_plan[] =
{
	{ SOAK_SYNC,     USBP_ROW_GC,  "sync"     },
	{ SOAK_NOREPLY,  USBP_ROW_GC,  "noreply"  },
	{ SOAK_STANDING, USBP_ROW_GC,  "standing" },
	{ SOAK_STREAM,   USBP_ROW_GC,  "stream"   },
	{ SOAK_STANDING, USBP_ROW_WII, "standing" },
	{ SOAK_STREAM,   USBP_ROW_WII, "stream"   },
};
#define USBP_SOAK_FLAVOURS (sizeof(usbp_soak_plan) / sizeof(usbp_soak_plan[0]))

/* "cost" per frame = what the thread spends on USB that frame:
 * sync/noreply = the exchange, standing = the OUT (the reply frames are
 * reported separately), stream = the post (wait + submit). "late" = the
 * frame's USB work overran the frame. */
static void usbp_soak(u32 pass)
{
	const u32 per_win = 60u * RA_USBP_WINDOW_SECS;
	u32 w, k, n, n_lat, n_rep, late, err, bad, uo, ui, next, now, cost;
	u32 kind, out_len, resp_len;
	bool reaped, want;
	s32 r;
	usbp_stats_t st;

	next = read32(HW_TIMER);
	for (w = 1; w <= RA_USBP_SOAK_SECS / RA_USBP_WINDOW_SECS; w++)
	{
		const u32 f = (w - 1) % USBP_SOAK_FLAVOURS;
		kind = usbp_soak_plan[f].kind;
		out_len = usbp_rows[usbp_soak_plan[f].row].out_len;
		resp_len = usbp_rows[usbp_soak_plan[f].row].resp_len;
		if (kind == SOAK_STREAM && RA_USBP_PADDED(out_len) > usbp_scap)
			kind = SOAK_NOREPLY;   /* no room for two buffers of this size */

		n = n_lat = n_rep = late = err = bad = 0;
		if (kind == SOAK_STANDING && usbp_standing_post() < 0)
			err++;
		for (k = 0; k < per_win; k++)
		{
			next += USBP_FRAME_TICKS;
			ui = 0;
			switch (kind)
			{
			case SOAK_SYNC:
			case SOAK_NOREPLY:
				r = usbp_exchange(RA_USBP_CMD_ECHO,
				                  kind == SOAK_SYNC ? USBP_SYNC : USBP_NOREPLY,
				                  out_len, resp_len, NULL, &uo, &ui);
				cost = uo + ui;
				break;
			case SOAK_STANDING:
				want = (k % USBP_STAND_EVERY) == USBP_STAND_EVERY - 1;
				r = usbp_standing_xfer(out_len, want, k + 1 < per_win, &uo, &ui);
				cost = want ? ui : uo;
				if (r == 0 && want && n_rep < USBP_SAMPLES)
					s_in[n_rep++] = ui;
				break;
			default:   /* SOAK_STREAM */
				r = usbp_stream_post(out_len, &cost, &ui, &reaped);
				if (r == 0 && reaped && n_lat < USBP_SAMPLES)
					s_in[n_lat++] = ui;
				break;
			}
			usbp_note_failure(r);
			if (r < 0)
				err++;
			else if (r > 0)
				bad++;
			else if (n < USBP_SAMPLES)
				s_pair[n++] = cost;

			now = read32(HW_TIMER);
			if ((s32)(now - next) >= 0)
			{
				late++;
				next = now;   /* overran the frame: don't try to catch up */
			}
			else
				udelay((int)usbp_us(next - now));   /* yields the CPU */
		}
		/* Close the window cleanly before logging (sync exchange). */
		if (kind == SOAK_STANDING && (per_win % USBP_STAND_EVERY) != 0)
		{
			r = usbp_standing_xfer(64, true, false, &uo, &ui);
			if (r != 0)
				err++;
		}
		if (kind == SOAK_STREAM)
			err += usbp_stream_drain(&n_lat);

		usbp_summarize3(0, kind == SOAK_STANDING ? n_rep : n_lat, n, &st);
		st.n_ok = n;
		st.n_err = err;
		st.n_bad = bad;
		usbp_account(&st);
		usbp_log("USBP soak#%u %s %u w=%u n=%u err=%u bad=%u late=%u "
		         "cost50/90/99/max=%u/%u/%u/%u us %s50/90=%u/%u | total pairs=%u "
		         "err=%u bad=%u worst=%u",
		         pass, usbp_soak_plan[f].name, out_len, w, n, err, bad, late,
		         st.p50, st.p90, st.p99, st.max,
		         kind == SOAK_STREAM ? "complete" : (kind == SOAK_STANDING ? "reply" : "-"),
		         st.in50, st.in90, tot_pairs, tot_err, tot_bad, tot_worst);
	}
}

/* High-speed latency proxy: when the game runs from a USB drive (EHCI for a
 * high-speed drive), time sector reads through the kernel's storage driver.
 * Each read = 3 transfers (CBW out, data in, CSW in) + the drive's own
 * latency, so it bounds the per-ioctl cost on the EHCI from above. Idle
 * pass only: the storage driver isn't thread-safe and the DI thread owns it
 * once the game runs. */
static void usbp_storage_probe(void)
{
	static const u32 bytes[2] = { 512, 4096 };
	u32 i, k, n, fails, nsec, t0;

	if (!ConfigGetConfig(NIN_CFG_USB))
	{
		usbp_log("USBP hs-proxy skipped: game is not on a USB drive");
		return;
	}
	for (i = 0; i < 2; i++)
	{
		nsec = bytes[i] / s_size;
		if (nsec == 0)
			nsec = 1;
		n = fails = 0;
		for (k = 0; k < RA_USBP_ITERS; k++)
		{
			t0 = read32(HW_TIMER);
			if (!USBStorage_ReadSectors(0, nsec, usbp_tx))
			{
				fails++;
				continue;
			}
			s_pair[n++] = usbp_us(read32(HW_TIMER) - t0);
		}
		usbp_sort(s_pair, n);
		usbp_log("USBP idle hs-proxy read %u B (%u x %u B sectors) ok=%u fail=%u "
		         "p50/90/99/max=%u/%u/%u/%u us (3 transfers each)",
		         nsec * s_size, nsec, s_size, n, fails, usbp_pct(n, 50),
		         usbp_pct(n, 90), usbp_pct(n, 99), n ? s_sort[n - 1] : 0);
	}
}

/* Wii (d2x) feasibility: the d2x usb-module keeps its own /dev/usb/ven fd
 * when it serves USB storage, so a Wii ra-module could only use USB if ven
 * takes a second client. Round 1 hinted it doesn't (with the loader still
 * holding ven, nothing the kernel sent reached the ESP) but never logged
 * the open. Also checks /dev/usb/oh0, the older API the other d2x bases
 * (37/38/56/57) expose for full-speed devices. Runs last in the idle pass:
 * if a second open disturbs the first fd, the SD log still records it. */
static void usbp_open_probe(void)
{
	static const char ven_path[] __attribute__((aligned(32))) = "/dev/usb/ven";
	static const char oh0_path[] __attribute__((aligned(32))) = "/dev/usb/oh0";
	s32 fd_ven2, fd_oh0, r;
	u32 uo, ui;

	fd_ven2 = IOS_Open(ven_path, IPC_OPEN_NONE);
	if (fd_ven2 >= 0)
		IOS_Close(fd_ven2);
	fd_oh0 = IOS_Open(oh0_path, IPC_OPEN_NONE);
	if (fd_oh0 >= 0)
		IOS_Close(fd_oh0);
	r = usbp_exchange(RA_USBP_CMD_ECHO, USBP_SYNC, 64, 8, NULL, &uo, &ui);
	usbp_log("USBP open-probe: 2nd /dev/usb/ven open=%d, /dev/usb/oh0 open=%d, "
	         "first fd still works=%s (r=%d)", fd_ven2, fd_oh0, r == 0 ? "yes" : "NO", r);
}

static bool usbp_setup(void)
{
	const ra_usbp_handoff_t *h = (const ra_usbp_handoff_t *)RA_USBP_HANDOFF_PHYS;
	u8 *b1;

	sync_before_read((void *)RA_USBP_HANDOFF_PHYS, sizeof(ra_usbp_handoff_t));
	usbp_log("USBP handoff: magic=%08x vid=%04x pid=%04x dev=%08x ep_in=%02x ep_out=%02x "
	         "mps=%u/%u if=%u n_ven=%u loader_err=%d",
	         h->magic, h->vid, h->pid, (u32)h->device_id, h->ep_in, h->ep_out,
	         h->mps_in, h->mps_out, h->interface, h->n_ven, h->loader_err);
	if (h->magic != RA_USBP_MAGIC_FOUND)
		return false;   /* the loader already told the user why */

	usbp_dev = h->device_id;
	usbp_ep_in = h->ep_in;
	usbp_ep_out = h->ep_out;

	/* Reuses the kernel's ven_fd (no-op when USB storage already opened it). */
	USB_Initialize();
	usbp_qin = mqueue_create(usbp_qin_heap, 1);
	usbp_qout = mqueue_create(usbp_qout_heap, 2);

	b1 = (u8 *)malloca(USBP_TX_MAX, 4096);
	if (b1)
	{
		usbp_sbuf[0] = usbp_tx;
		usbp_sbuf[1] = b1;
		usbp_scap = USBP_TX_MAX;
	}
	else
	{
		usbp_sbuf[0] = usbp_tx;
		usbp_sbuf[1] = usbp_tx + 4096;
		usbp_scap = 4095;   /* each half stays inside one page */
	}
	usbp_ready = (usbp_qin >= 0 && usbp_qout >= 0);

	usbp_log("USBP hello: ven_fd=%d qin=%d qout=%d stream_buf=%u%s usb_storage=%u "
	         "s_size=%u iters=%u",
	         USB_GetVenFd(), usbp_qin, usbp_qout, usbp_scap,
	         b1 ? " (heap)" : " (halves)", ConfigGetConfig(NIN_CFG_USB) ? 1 : 0,
	         s_size, RA_USBP_ITERS);
	return usbp_ready;
}

void RA_USBP_Idle(void)
{
	/* The game isn't running yet, so FatFs is ours for this pass. */
	usbp_fil = (FIL *)malloca(sizeof(FIL), 32);
	if (usbp_fil)
	{
		usbp_sdlog = f_open_char(usbp_fil, "/usbprobe.log", FA_CREATE_ALWAYS | FA_WRITE);
		if (usbp_sdlog != FR_OK)
		{
			free(usbp_fil);
			usbp_fil = NULL;
		}
	}

	if (usbp_setup())
	{
		usbp_matrix("idle", 1, true);
		usbp_storage_probe();
		usbp_open_probe();
		usbp_log("USBP idle done: ok=%u first_err kind=%d code=%d - releasing game "
		         "(hold %u ms)", tot_pairs, usbp_first_kind, usbp_first_err,
		         RA_USBP_IDLE_HOLD_MS);
	}

	if (usbp_fil)
	{
		f_close(usbp_fil);
		free(usbp_fil);
		usbp_fil = NULL;
	}

	if (usbp_ready)
	{
		usbp_publish(2, USBP_NROWS, NULL, 0, tot_err + tot_bad);   /* + sdlog */
		mdelay(RA_USBP_IDLE_HOLD_MS);   /* let the user read the loader screen */
	}
}

static u32 usbp_thread(void *arg)
{
	u32 pass;
	(void)arg;

	mdelay(RA_USBP_GAME_DELAY_MS);   /* let the game boot */
	usbp_log("USBP in-game probe start (matrix + %us rotating soak per pass)",
	         RA_USBP_SOAK_SECS);
	for (pass = 1; ; pass++)
	{
		usbp_matrix("game", pass, false);
		usbp_soak(pass);
	}
	return 0;
}

void RA_USBP_Start(void)
{
	/* Borrows the RA thread's stack region (kernel.ld): only valid while
	 * the RA module itself is not running. */
	extern char __ra_stack_addr, __ra_stack_size;

	if (!usbp_ready)
		return;
	if (ConfigGetConfig(NIN_CFG_RA))
	{
		usbp_log("USBP: RetroAchievements is ON - in-game probe skipped (turn RA off)");
		return;
	}
	/* 0x78 = the highest IOS lets the kernel create (0x7F/0x7C were refused,
	 * round 4), and 0x50 measured identical: priority is not a factor. */
	usbp_thread_id = do_thread_create(usbp_thread, (u32 *)&__ra_stack_addr,
	                                  (u32)(&__ra_stack_size), RA_USBP_PRIO);
	if ((s32)usbp_thread_id >= 0)
		thread_continue(usbp_thread_id);
	else
		usbp_log("USBP: thread_create failed (%d) - no in-game probe", (s32)usbp_thread_id);
}

#else

void RA_USBP_Idle(void) {}
void RA_USBP_Start(void) {}

#endif /* RA_USB_PROBE */
