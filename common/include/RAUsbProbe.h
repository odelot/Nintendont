/*
 * RAUsbProbe.h — USB transport SPIKE (loader <-> kernel shared definitions).
 *
 * Measures the cost of one request/response pair over USB between the
 * Nintendont kernel (Starlet, /dev/usb/ven) and an ESP32-S3 running the echo
 * firmware in c:/dev/wii/usb-spike-esp. Plan: c:/dev/wii/PLAN-USB-SPIKE.md.
 *
 * Isolated and disposable: nothing here touches the EXI production path.
 * With RA_USB_PROBE 0 the loader and kernel build exactly as before.
 *
 * Flow:
 *   1. Loader (PPC), right before launching the game, finds the ESP on
 *      /dev/usb/ven by VID/PID, reads its descriptors and writes the handoff
 *      below (same mechanism the USB-storage path uses at 0x932C1000).
 *   2. Kernel, before releasing the game, runs the measurement matrix once
 *      with the system idle and publishes a summary in the result block,
 *      which the loader draws on its status screen.
 *   3. Kernel background thread keeps measuring with the game running
 *      (matrix + 60 Hz soak) and ships every report line to the ESP, which
 *      prints it on its UART console.
 *
 * The RetroAchievements setting must be OFF during the spike (the probe
 * thread borrows the RA thread's stack, and RA On requires the EXI adapter).
 */

#ifndef __RA_USB_PROBE_H__
#define __RA_USB_PROBE_H__

/* Master switch for the spike (loader + kernel): the measurement matrix and
 * soak. Off in production builds; the handoff below is shared with the link. */
#define RA_USB_PROBE            0

/* PRODUCTION: RetroAchievements over USB (kernel/ra_usb_link.c). The loader
 * looks for the adapter (VID:PID below) and writes the handoff; when it
 * finds one, the kernel carries the RA protocol over USB, otherwise it stays
 * on the memory-card adapter in Slot B (EXI). Adapter firmware: wii-ra-adapter
 * built with CONFIG_RA_LINK_USB. Plan: c:/dev/wii/PLAN-USB-SPIKE.md §12. */
#define RA_USB_LINK             1

/* ESP32-S3 echo device. 0x303A = Espressif; the PID is a free test value
 * (0x4001 is esp_tinyusb's default CDC PID — avoided so Windows can't reuse a
 * cached CDC driver binding). Must match usb-spike-esp/main/main.c. */
#define RA_USBP_VID             0x303A
#define RA_USBP_PID             0x4052

/* Handoff (loader -> kernel) and result (kernel -> loader) blocks, right after
 * the 44-byte USB-storage handoff at 0x932C1000. Verified unused. */
#define RA_USBP_HANDOFF_PHYS    0x132C1100
#define RA_USBP_RESULT_PHYS     0x132C1120
#define RA_USBP_HANDOFF_PPC     0x932C1100
#define RA_USBP_RESULT_PPC      0x932C1120
#define RA_USBP_AREA_LEN        0x60    /* handoff (32) + result (64) */

#define RA_USBP_MAGIC_FOUND     0x55534250  /* 'USBP' */
#define RA_USBP_MAGIC_NOTFOUND  0x5553424E  /* 'USBN' (loader looked, no device) */
#define RA_USBP_MAGIC_RESULT    0x55534252  /* 'USBR' */

typedef struct
{
	u32 magic;          /* RA_USBP_MAGIC_FOUND / _NOTFOUND / 0 (loader never ran) */
	u16 vid;
	u16 pid;
	s32 device_id;      /* USBV5 device id (libogc's "fd"), global across fds */
	u8  ep_in;
	u8  ep_out;
	u8  interface;
	u8  n_ven;          /* devices listed on /dev/usb/ven when the loader looked */
	u16 mps_in;
	u16 mps_out;
	s32 loader_err;     /* 0 = ok, else the failing step (see ra_usb_probe.c) */
	u32 pad[3];
} ra_usbp_handoff_t;    /* 32 bytes */

typedef struct
{
	u32 magic;          /* RA_USBP_MAGIC_RESULT once the kernel wrote it */
	s32 stage;          /* 1 = measuring, 2 = done, < 0 = failed (see kernel) */
	u32 row;            /* matrix row being measured (1-based) */
	u32 key_p50_us;     /* 2048-out / 8-in pair (the GameCube regime row) */
	u32 key_p90_us;
	u32 key_max_us;
	u32 floor_p50_us;   /* 64-out / 8-in pair (pure IPC + scheduling cost) */
	u32 errors;         /* transfer errors + validation failures, all rows */
	s32 ven_fd;         /* kernel's /dev/usb/ven fd (< 0 = IOS_Open failed) */
	s32 first_err;      /* raw return of the first failing exchange */
	s32 first_err_kind; /* usbp_exchange code: -1 OUT, -2 IN, 1 bad reply, 2 ESP flag */
	u32 ok_pairs;       /* exchanges that passed, all rows */
	s32 sdlog;          /* FatFs result of writing /usbprobe.log (0 = ok) */
	u32 pad[3];
} ra_usbp_result_t;     /* 64 bytes */

/* ---- Wire protocol (console = host, ESP = device) -------------------------
 * Every exchange is one bulk OUT (request) followed by one bulk IN (reply).
 * Multi-byte fields are big-endian on the wire.
 *
 * Request:  [0]'U' [1]cmd [2..3]out_len [4..5]resp_len [6]flags [7]0 [8..11]seq
 *           flags bit0 = NO_REPLY: the ESP validates but sends nothing back
 *           (measures the "read the reply only when needed" model)
 *           ECHO: bytes [12, out_len) = (u8)(seq + i)
 *           LOG : bytes [12, out_len) = ASCII text, printed by the ESP
 * Reply:    [0]'u' [1]status [2..3]rx_len [4..7]seq
 *           bytes [8, resp_len) = (u8)((seq ^ 0xA5) + i)
 *
 * Neither side ever sends a message whose length is a multiple of 64 (the
 * full-speed bulk packet size): such a message gets one extra pad byte. So
 * every transfer ends in a short packet, which is what terminates it on both
 * ends — no ZLP handling, and the host can always read into a buffer larger
 * than the reply (exactly how a production reply of ~10 bytes would work). */
#define RA_USBP_REQ_MAGIC       0x55    /* 'U' */
#define RA_USBP_RESP_MAGIC      0x75    /* 'u' */
#define RA_USBP_CMD_ECHO        1
#define RA_USBP_CMD_LOG         2
#define RA_USBP_FLAG_NO_REPLY   0x01
#define RA_USBP_HDR_LEN         12
#define RA_USBP_RESP_HDR_LEN    8

#define RA_USBP_ST_OK           0
#define RA_USBP_ST_BAD_PATTERN  1
#define RA_USBP_ST_BAD_LEN      2
#define RA_USBP_ST_BAD_HDR      3

#define RA_USBP_PADDED(n)       ((n) + (((n) & 63u) == 0))

#endif /* __RA_USB_PROBE_H__ */
