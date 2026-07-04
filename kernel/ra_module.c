/*
 * ra_module.c — RetroAchievements memory server (Nintendont kernel side).
 *
 * PHASE 0 — proof of life:
 *   1. Background thread spins up and blinks the disc-slot LED (alive).
 *   2. Reads HW_EXICTRL to report whether the EXI bus master is enabled.
 *   3. Loops doing IDENTIFY round-trips to the ESP32 in Slot B (EXI chan 1),
 *      encoding the result on the LED and, on success, sending a DEBUG_LOG
 *      line the ESP prints over its USB-Serial (COM4) monitor.
 *
 * This validates, on real hardware and without writing any HW_EXICTRL bit,
 * that the real Hollywood EXI controller is free and drivable from the
 * Nintendont kernel. Later phases add MEM1 SNAPSHOT, the watchlist state
 * machine, the GameCube RA hash, and VBlank sync.
 *
 * See c:/dev/gamecube/Nintendont/RA_PORT_PLAN.md for the full plan.
 */

#include "global.h"
#include "common.h"
#include "string.h"
#include "syscalls.h"
#include "gc_ra_protocol.h"
#include "ra_module.h"
#include "RealDI.h"   /* ReadRealDisc */
#include "ISO.h"      /* ISORead, ISOInit */
#include "md5.h"      /* md5_init / md5_append / md5_finish */

/* Game-image read-backend selectors (set during kernel init). */
extern u32 RealDiscCMD;   /* != 0 => physical disc (RealDI.c) */
extern u32 FSTMode;       /* != 0 => folder/FST game (FST.c) */

/* Nintendont's SD/USB disk code (diskio.c, RealDI.c) drives GPIO_SLOT_LED as an
 * access indicator when this is set. We suppress it during the achievement
 * celebration so our blink isn't masked by streaming activity (see led_celebrate). */
extern bool access_led;

/* ------------------------------------------------------------------------
 * EXI wiring — real Hollywood controller (Starlet view).
 *
 * Same base the Wii d2x ra-module/exi.c uses (EXI_REG_BASE 0xd806800).
 * NOTE: this is the REAL controller, distinct from Nintendont's emulated
 * EXI register block at 0x13026800 (which only the patched game touches).
 * Channel stride 0x14; per-channel layout CSR/MAR/LEN/CR/DATA at +0/4/8/c/10.
 * ------------------------------------------------------------------------ */
#define RA_EXI_BASE        0x0d806800
#define RA_EXI_STRIDE      0x14
#define RA_EXI_CSR(ch)     (RA_EXI_BASE + (ch) * RA_EXI_STRIDE + 0x00)
#define RA_EXI_CR(ch)      (RA_EXI_BASE + (ch) * RA_EXI_STRIDE + 0x0C)
#define RA_EXI_DATA(ch)    (RA_EXI_BASE + (ch) * RA_EXI_STRIDE + 0x10)

#define RA_HW_EXICTRL      0x0d800070
#define RA_EXICTRL_ENABLE  1

/* ESP32 in Slot B, CS0, 8 MHz — identical to the Wii. */
#define RA_CHAN            1
#define RA_DEV             0
#define RA_FREQ            0x30   /* EXI_SPEED8MHZ (CPR bits 4..6) */

/* CR bit 0 (TSTART/busy) completion poll budget. A real transaction
 * completes in microseconds; this cap only matters if the bus master is
 * off and the bit never clears, in which case we bail quickly. */
#define RA_CR_SPIN_MAX     200000u

static u32 ra_thread_id = 0;

/* ------------------------------------------------------------------------
 * Slot-LED diagnostic (GPIO bit 5). Set up independently of NIN_CFG_LED so
 * the proof-of-life works regardless of the user's LED config. Mirrors the
 * ENABLE/DIR/OWNER setup in main.c. Nintendont's disk code also toggles
 * this bit, so during disc loading our pattern may be interleaved with
 * access blinks — the COM4 DEBUG_LOG is the unambiguous channel.
 * ------------------------------------------------------------------------ */
static void led_setup(void)
{
	set32(HW_GPIO_ENABLE, GPIO_SLOT_LED);
	set32(HW_GPIO_DIR, GPIO_SLOT_LED);     /* DIR=1 => OUTPUT (the bug: was clear=input, so OUT writes didn't drive the pin; diag showed dr=0 + ot toggling + LED stuck on) */
	clear32(HW_GPIO_OWNER, GPIO_SLOT_LED); /* OWNER=0 => Starlet drives via HW_GPIO_OUT */
}
static void led_on(void)  { set32(HW_GPIO_OUT, GPIO_SLOT_LED); }
static void led_off(void) { clear32(HW_GPIO_OUT, GPIO_SLOT_LED); }

static void led_blink(int count, int ms)
{
	int i;
	for (i = 0; i < count; i++) {
		led_on();  mdelay(ms);
		led_off(); mdelay(ms);
	}
}

/* ------------------------------------------------------------------------
 * Achievement celebration — non-blocking, driven over the SNAPSHOT loop so the
 * LED blink, screen overlay and rumble all run together for a couple seconds
 * without stalling the snapshot pipeline (the old blocking led blink would have
 * frozen the overlay redraw). On unlock the loop sets ra_celeb = RA_CELEB_FRAMES
 * and counts down, applying each effect per iteration. Each gated by its toggle.
 * ------------------------------------------------------------------------ */
/* 3 LED blinks to match the Wii d2x ra_led_celebrate (~150 ms on/off). The loop
 * runs ~50 Hz, so RA_BLINK_HALF=8 iterations ≈ 150 ms per half-pulse; total =
 * 3 blinks * 2 halves * 8 = 48 iterations. */
#define RA_BLINK_HALF      8u
#define RA_CELEB_FRAMES   (3u * 2u * RA_BLINK_HALF)   /* = 48 loop iterations */
#define RA_RUMBLE_FRAMES   16u   /* (rumble disabled) first part only, if ever re-enabled */
static u32 ra_celeb = 0;

#if RA_USE_RUMBLE
/* Kernel-owned rumble flag PADReadGC polls (MEM1, codehandler cheats area). */
static vu32 * const ra_rumble_flag = (vu32*)RA_RUMBLE_FLAG_PHYS;
static void ra_set_rumble(u32 on)
{
	*ra_rumble_flag = on;
	sync_after_write((void*)ra_rumble_flag, 4);
}
#endif

#if RA_USE_OVERLAY
/* Trophy overlay (kernel side): just raise/lower a flag the codehandler polls.
 * The codehandler (PPC, per frame) draws the 32x32 gold badge into both the
 * current and previous VI_TFBL whenever this flag is non-zero — mirrors the
 * working WiiFlow ra_trophy_hook. Flag lives in the codehandler cheatdata
 * scratch (fixed at 0x1008). */
static vu32 * const ra_trophy_flag = (vu32*)RA_TROPHY_FLAG_PHYS;
static void ra_set_trophy(u32 on)
{
	*ra_trophy_flag = on;
	sync_after_write((void*)ra_trophy_flag, 4);
}
#endif

/* ------------------------------------------------------------------------
 * Minimal EXI driver — direct register access (kernel/SVC mode).
 *
 * CSR: (freq & 0x70) | (1 << (dev + 7))  → set CLK + chip-select.
 * CR : TSTART(bit0) | ((len-1)&3)<<4 (TLEN) | (write? 1<<2 : 0) (RW).
 * Immediate transfers move 1..4 bytes via the DATA register, packed
 * big-endian (matches the Wii ARM, which is also big-endian).
 * ------------------------------------------------------------------------ */
static int ra_wait_cr(int ch)
{
	u32 spins = 0;
	while (read32(RA_EXI_CR(ch)) & 1u) {
		if (++spins >= RA_CR_SPIN_MAX)
			return -1;
	}
	return 0;
}

static void ra_select(int ch)
{
	write32(RA_EXI_CSR(ch), (RA_FREQ & 0x70) | (1u << (RA_DEV + 7)));
}

static void ra_deselect(int ch)
{
	write32(RA_EXI_CSR(ch), 0);
}

static int ra_imm_write(int ch, const u8 *buf, u32 len)
{
	while (len > 0) {
		u32 n = (len >= 4) ? 4 : len;
		u32 v = 0, i;
		for (i = 0; i < n; i++)
			v |= ((u32)buf[i]) << ((3 - i) * 8);
		write32(RA_EXI_DATA(ch), v);
		write32(RA_EXI_CR(ch), 1u | (((n - 1) & 3) << 4) | (1u << 2));
		if (ra_wait_cr(ch) < 0) return -1;
		buf += n;
		len -= n;
	}
	return 0;
}

static int ra_imm_read(int ch, u8 *buf, u32 len)
{
	while (len > 0) {
		u32 n = (len >= 4) ? 4 : len;
		u32 v, i;
		write32(RA_EXI_CR(ch), 1u | (((n - 1) & 3) << 4));  /* RW=read */
		if (ra_wait_cr(ch) < 0) return -1;
		v = read32(RA_EXI_DATA(ch));
		for (i = 0; i < n; i++)
			buf[i] = (v >> ((3 - i) * 8)) & 0xFF;
		buf += n;
		len -= n;
	}
	return 0;
}

/* Half-duplex transaction: select → write → read → deselect. */
static int ra_xfer(const void *tx, u32 tx_len, void *rx, u32 rx_len)
{
	int r = 0;
	ra_select(RA_CHAN);
	if (tx && tx_len) r = ra_imm_write(RA_CHAN, (const u8 *)tx, tx_len);
	if (r == 0 && rx && rx_len) r = ra_imm_read(RA_CHAN, (u8 *)rx, rx_len);
	ra_deselect(RA_CHAN);
	return r;
}

/* ---------- Phase B: device-INT handshake ----------
 * The ESP asserts its INT pin (memcard slot pin 2) after it has prepared a
 * response (exi_spi_prepare_response -> exi_spi_arm). The Hollywood EXI
 * controller latches that on EXI_CSR bit 1 (RW1C, sticky). So: write the
 * request (CS-low), wait for the latched INT, clear it, then read the
 * response in a SECOND CS-low. This avoids the arm-after-prepare race that
 * makes single-CS-low reads return idle 0xFF for freshly prepared responses
 * (GET_CHUNK, SNAPSHOT events). Mirrors the Wii ra-module ra_send_phase_b. */
#define RA_CSR_EXI_IRQ  0x0002u
#define RA_CSR_TC_IRQ   0x0008u
#define RA_CSR_EXT_IRQ  0x0800u

static int ra_wait_int(int ch, u32 timeout_ms)
{
	u32 us = 0, budget = timeout_ms * 1000u;
	/* Poll at 250 us instead of 1 ms: the ESP asserts the INT quickly for the
	 * small ADDR_RESPONSE passes, so 1 ms granularity wasted up to ~1 ms/pass
	 * (~3-4 ms/snapshot over the multi-pass). udelay() still yields (timer +
	 * mqueue_recv), so the kernel main loop keeps servicing the game during the
	 * longer waits (e.g. the ~6-10 ms of a full do_frame). */
	while (!(read32(RA_EXI_CSR(ch)) & RA_CSR_EXI_IRQ)) {
		udelay(250);
		us += 250;
		if (us >= budget) return -1;
	}
	return 0;
}

static void ra_clear_int(int ch)
{
	u32 csr = read32(RA_EXI_CSR(ch));
	csr &= ~(RA_CSR_EXI_IRQ | RA_CSR_TC_IRQ | RA_CSR_EXT_IRQ);
	csr |= RA_CSR_EXI_IRQ;   /* RW1C: write 1 to clear just the device-INT bit */
	write32(RA_EXI_CSR(ch), csr);
}

static int ra_phase_b(const void *tx, u32 tx_len, void *rx, u32 rx_len)
{
	int ret;

	ra_clear_int(RA_CHAN);                 /* drop any stale latch */

	/* WRITE phase */
	ra_select(RA_CHAN);
	if (tx && tx_len) {
		if (ra_imm_write(RA_CHAN, (const u8 *)tx, tx_len) < 0) {
			ra_deselect(RA_CHAN);
			return -1;
		}
	}
	ra_deselect(RA_CHAN);

	/* WAIT for the ESP to prepare + arm + assert INT. */
	if (ra_wait_int(RA_CHAN, 100) < 0)
		return -2;
	ra_clear_int(RA_CHAN);

	/* SETTLE: ESP-IDF SPI slave may finish DMA setup a touch after INT. */
	mdelay(1);

	/* READ phase — response now armed at the ESP's tx_buf[0]. */
	ra_select(RA_CHAN);
	ret = (rx && rx_len) ? ra_imm_read(RA_CHAN, (u8 *)rx, rx_len) : 0;
	ra_deselect(RA_CHAN);
	return ret;
}

/* Fire-and-forget DEBUG_LOG so the ESP prints `msg` on its Serial monitor.
 * Single CS-low write, no read/INT wait — the ESP parses the command from
 * its rx buffer once the transaction completes. */
static void ra_debug(const char *msg)
{
	u8 buf[sizeof(ra_gc_header_t) + 1 + 64];
	ra_gc_header_t *h = (ra_gc_header_t *)buf;
	u32 n = 0;
	while (msg[n] && n < 64) n++;

	h->magic       = RA_MAGIC_GC_TO_ESP;
	h->command     = RA_CMD_DEBUG_LOG;
	h->payload_len = (u16)(1 + n);
	buf[sizeof(ra_gc_header_t)] = (u8)n;
	memcpy(buf + sizeof(ra_gc_header_t) + 1, msg, n);

	ra_select(RA_CHAN);
	ra_imm_write(RA_CHAN, buf, sizeof(ra_gc_header_t) + 1 + n);
	ra_deselect(RA_CHAN);

	/* Fire-and-forget with no INT/sync: the ESP needs a moment to consume this
	 * transaction (a ~64-char Serial.print at 250000 baud ≈ 2.5 ms) and re-arm
	 * its SPI slave before the next one. 150 ms was wildly over-generous (a
	 * relic from the pre-protocol-stable days) and, since the heartbeat fires
	 * this once/second INSIDE the snapshot loop, it was eating ~16% of df/s.
	 * 10 ms is ~4x the Serial cost — plenty. Worst case if too short: an
	 * occasional dropped DEBUG line (diagnostics only, never a hang). */
	mdelay(10);
}

/* One IDENTIFY round-trip.
 * Returns 0 = OK (magic + device id match), 1 = magic OK but wrong id,
 * 2 = bad magic / EXI error. (Unused since the GET_CHUNK Phase B conversion
 * dropped the prime; kept for diagnostics/future use.) */
static int ra_identify(void) __attribute__((unused));
static int ra_identify(void)
{
	ra_gc_header_t hdr;
	u8  rx[sizeof(ra_esp_header_t) + sizeof(u32)];
	u32 device_id;

	hdr.magic       = RA_MAGIC_GC_TO_ESP;
	hdr.command     = RA_CMD_IDENTIFY;
	hdr.payload_len = 0;
	memset(rx, 0, sizeof(rx));

	if (ra_xfer(&hdr, sizeof(hdr), rx, sizeof(rx)) < 0)
		return 2;
	if (rx[0] != RA_MAGIC_ESP_TO_GC)
		return 2;

	memcpy(&device_id, rx + sizeof(ra_esp_header_t), sizeof(device_id));
	return (device_id == RA_DEVICE_ID) ? 0 : 1;
}

/* ========================================================================
 * Phase 3 — post-boot watchlist + SNAPSHOT (reads the running game's MEM1).
 *
 * Ported from the Wii d2x ra-module (main.c) but adapted for Nintendont:
 *   - EXI via the direct half-duplex ra_xfer (no Phase B INT handshake yet;
 *     add it in a later step if multi-pass races appear).
 *   - MEM1 read via 32-bit aligned read32 + sync_before_read (Starlet can't
 *     reliably do 8/16-bit MEM1 reads). rcheevos GameCube addresses are
 *     0-based (0..0x017FFFFF) mapping to real 0x80000000 (rc_consoles.c).
 * ======================================================================== */
static char ra_hexd(u8 n);   /* defined later */

/* Match the canonical d2x ra-module (Phase D2 protocol, gc_ra_protocol.h). */
#define RA_MAX_WATCH       RA_MAX_WATCH_ADDRS   /* 6144 */
#define RA_ADDR_QUERY_MAX  512
/* Longest ESP response: ADDR_QUERY/APPEND = 6(hdr)+4(seq+count)+512*4 = 2058,
 * rounded up to 32. Phase B reads this whole buffer every snapshot. */
#define RA_RESP_LEN        (((6 + 4 + RA_ADDR_QUERY_MAX * 4) + 31) & ~31u)

static u32 ra_watchlist[RA_MAX_WATCH] __attribute__((aligned(32)));
static u16 ra_watch_count = 0;
static u32 ra_frame_counter = 0;   /* delivery count (heartbeat/diag) */

/* Frame clock shipped in the SNAPSHOT header so the ESP's rc_client_do_frame
 * debt catch-up advances at the right rate.
 *
 * Two sources, selected automatically (see RA_USE_VBI in ra_module.h):
 *   1. VBlank counter (preferred): the codehandler bumps a u32 at MEM1 0x2FF8
 *      once per game frame; we poll it. This is a TRUE per-game-frame clock —
 *      identical to how the Dolphin emulator drives RA, the most faithful to
 *      how achievements (esp. frame-count timers) are authored.
 *   2. HW_TIMER 60 Hz wall-clock fallback (Hollywood timer, 1 tick ≈ 526.7 ns):
 *      used until/unless the VBI counter proves alive, so we always have a sane
 *      monotonic clock even if the game lacks the OSSleepThread hook pattern. */
#define RA_FRAME_US     16667u
#define RA_FRAME_TICKS  ((RA_FRAME_US * 1000u) / 527u)   /* ≈ 31626 ticks/frame */
static u32 ra_game_frame = 0;
static u32 ra_start_tick = 0;

#if RA_USE_VBI
/* VBlank counter state. ra_vbi_live latches once the PPC counter has advanced,
 * after which we trust it as the frame source (and never fall back, so a paused
 * game correctly stops do_frame instead of the wall-clock running on). */
static u32 ra_vbi_last  = 0;   /* last counter value read */
static u32 ra_vbi_base  = 0;   /* counter value when it first went live */
static int ra_vbi_live  = 0;   /* 1 once the counter has been seen to advance */

/* Read the per-frame VBlank counter the codehandler increments at MEM1 0x1004
 * (codehandler cheatdata scratch). Single fixed address polled repeatedly → the
 * Starlet D-cache would pin a stale value, so invalidate the line first (safe:
 * 0x1004 is in the reserved codehandler area, never touched by the DI thread).
 * 0-based physical. */
static u32 ra_read_vbi(void)
{
	u32 a = RA_VBI_COUNTER_PHYS & ~3u;
	sync_before_read((void*)(a & ~0x1Fu), 0x20);
	return read32(a);
}
#endif
static u32 ra_pending_q[RA_ADDR_QUERY_MAX] __attribute__((aligned(32)));
static u16 ra_pending_qn = 0;

/* Phase D2 verified-sync state. ra_wl_seq = seq of the last watchlist mutation
 * we applied (echoed in every SNAPSHOT). resync_* set when the ESP publishes a
 * WATCHLIST_UPDATE (full re-fetch needed). ra_ach_fired set on an ACHIEVEMENT
 * event so the loop can blink + log without doing EXI inside the parser. */
static u16 ra_wl_seq = 0;
static u8  ra_resync_pending = 0;
static u16 ra_resync_seq = 0;
static u8  ra_ach_fired = 0;

/* SNAPSHOT tx: 8192 like the canonical d2x ra-module — Phase C snapshots
 * carry flat values + the chain window (bitmap + blob) + dp sections; the
 * window budget in ra_send_snapshot is derived from sizeof() this buffer. */
static u8 ra_snap_tx[8192]        __attribute__((aligned(32)));
static u8 ra_snap_rx[RA_RESP_LEN] __attribute__((aligned(32)));
/* GET_CHUNK response via Phase B: 6(FF pad)+6(esp_hdr)+hdr+payload.
 *   watchlist: 6 + 6 + 6 + 1024×4 = 4114
 *   chain:     6 + 6 + 8 + 512×8  = 4116  (RA_CHAIN_CHUNK_NODES nodes)
 * The leading 6-byte pad (GC_WRITE_PADDING the ESP prepends) lands at the start
 * of the Phase B read, so the buffer needs the extra 6 vs the old 2-tx path. */
static u8 ra_chunk_rx[6 + 6 + 8 + RA_WATCHLIST_CHUNK_ADDRS * 4] __attribute__((aligned(32)));

/* ------------------------------------------------------------------------
 * Phase C chain descriptor table + per-frame walk state (faithful port of
 * the canonical d2x ra-module). Fetched once per game via
 * RA_CMD_GET_CHAIN_CHUNK (after the watchlist); node semantics in
 * gc_ra_protocol.h. node_count==0 => Phase C off (pure legacy).
 * ------------------------------------------------------------------------ */
#define RA_CHAIN_PARENT_NONE  0xFFFF

/* Console-side node cap. SMALLER than the protocol's RA_MAX_CHAIN_NODES
 * (3072, sized for SMG+LB on the Wii): the Nintendont kernel data region
 * (kernel.ld, 512KB) has only ~69KB of .bss headroom, and 3072-node arrays
 * cost ~71KB. GC sets are far smaller (Prime-class ≈ hundreds of nodes), so
 * 2048 (~48KB) fits with margin. Caps may differ per console: a table bigger
 * than OUR cap is rejected at fetch (-103) -> clean legacy fallback on both
 * sides (the ESP arms Phase C only when the console fetches the LAST chunk). */
#define RA_GC_CHAIN_NODES  2048

typedef struct __attribute__((packed)) {
	u32 operand;   /* const offset/mask/immediate, or root absolute addr */
	u16 parent;    /* earlier node index, or RA_CHAIN_PARENT_NONE */
	u8  op;        /* bits 0-4 RA_CN_OP_*, bits 5-6 width-1, bit 7 shipped */
	u8  psize;     /* RA_CN_SZ_* transform applied to the parent value */
} ra_chain_node_t;

static ra_chain_node_t ra_chain_nodes[RA_GC_CHAIN_NODES] __attribute__((aligned(32)));
static u32 ra_chain_values[RA_GC_CHAIN_NODES];         /* LE-pack raw per node */
static u16 ra_chain_ship_node[RA_GC_CHAIN_NODES];      /* shipped idx -> node idx */
static u8  ra_chain_node_valid[RA_GC_CHAIN_NODES / 8]; /* bitmap per NODE */
static u16 ra_chain_node_count = 0;
static u16 ra_chain_shipped = 0;
static u16 ra_chain_win_next = 0;                      /* rotating window cursor */

/* Phase D: per-node frame history so dp edges (psize high bits) can read the
 * parent's PREVIOUS-frame value (rcheevos DELTA) or last-DIFFERENT value
 * (rcheevos PRIOR). Sound because the ESP's snapshot queue guarantees every
 * walked frame is evaluated (walks == do_frames), so "previous walk" IS the
 * evaluator's delta. Updated per node inside the walk (rcheevos update rule);
 * dp edges are INVALID until the second walk (ra_chain_have_prev). The dp
 * verification list is the distinct (parent,kind) pairs found scanning the
 * table in order — the ESP derives the identical list from its own table. */
static u32 ra_chain_values_prev[RA_GC_CHAIN_NODES];   /* value at previous walk */
static u32 ra_chain_prior[RA_GC_CHAIN_NODES];         /* last DIFFERENT value */
static u8  ra_chain_valid_prev[RA_GC_CHAIN_NODES / 8];/* validity at previous walk —
                                                        * ships as a bitmap after the
                                                        * dp values so the ESP SKIPS
                                                        * the compare for parents that
                                                        * did not resolve (unloaded
                                                        * pointers; safe: their whole
                                                        * subtree evaluates as legacy
                                                        * zeros anyway) */
static u8  ra_chain_have_prev = 0;
static u16 ra_chain_dp_node[RA_MAX_DP_PARENTS];        /* dp parent node index */
static u8  ra_chain_dp_kind[RA_MAX_DP_PARENTS];        /* RA_CN_DP_DELTA/PRIOR */
static u16 ra_chain_dp_count = 0;

/* Read one byte of GameCube system RAM (MEM1, 24 MB at 0x80000000).
 *
 * NO per-read sync_before_read: a DC-invalidate here races the kernel's DI
 * thread, which is memcpy'ing the game into MEM1 during boot — invalidating a
 * line it has dirtied discards that write and corrupts the loading game (froze
 * the boot). The canonical Wii ra-module reads MEM1 directly for the same
 * reason; scattered snapshot reads (hundreds/frame) thrash the D-cache enough
 * that values are effectively fresh.
 *
 * Clamp to the real 24 MB window: a stray watchlist address beyond MEM1 (e.g.
 * garbage from a bad chunk) would otherwise read 0x81800000+ and data-abort
 * the kernel. 32-bit aligned read + big-endian byte extract. */
static u8 ra_read_mem1(u32 addr)
{
	u32 off = addr & 0x01FFFFFFu;
	u32 w;
	if (off >= 0x01800000u) return 0;          /* outside GC MEM1 — skip safely */
	/* MEM1 is at PHYSICAL 0-based from the Starlet (Nintendont's P2C macro =
	 * &0x7FFFFFFF strips the 0x80000000 Broadway cached-view bit before any
	 * MEM1 access; the canonical Wii ra-module also reads 0-based). Reading
	 * 0x80000000|off instead hits an unmapped/HW region on the Starlet and
	 * data-aborts — which is why ANY non-zero watchlist crashed the game while
	 * n=0 (no reads) ran for 16 min. */
	w = read32(off & ~3u);
	return (u8)(w >> ((3 - (off & 3)) * 8));
}

/* ------------------------------------------------------------------------
 * Phase C — chain walker (faithful port of the canonical d2x ra-module;
 * mirrors the ESP resolver EXACTLY — see gc_ra_protocol.h for semantics).
 * Only GC difference: address validation is MEM1-only (24 MB, 0-based);
 * there is no MEM2 in the GameCube rcheevos memory map. switch() is fine
 * here (unlike d2x, the Nintendont kernel loads .rodata normally) but the
 * if/else shape is kept so the two files stay diffable.
 * ------------------------------------------------------------------------ */
static u32 ra_chain_xform(u32 v, u8 psize)
{
	if (psize == RA_CN_SZ_32)    return v;
	if (psize == RA_CN_SZ_32_BE) return ((v & 0xFF000000u) >> 24) |
	                                    ((v & 0x00FF0000u) >>  8) |
	                                    ((v & 0x0000FF00u) <<  8) |
	                                    ((v & 0x000000FFu) << 24);
	if (psize == RA_CN_SZ_8)     return v & 0x000000FFu;
	if (psize == RA_CN_SZ_16)    return v & 0x0000FFFFu;
	if (psize == RA_CN_SZ_24)    return v & 0x00FFFFFFu;
	if (psize == RA_CN_SZ_16_BE) return ((v & 0xFF00u) >> 8) | ((v & 0x00FFu) << 8);
	if (psize == RA_CN_SZ_24_BE) return ((v & 0xFF0000u) >> 16) | (v & 0x00FF00u) |
	                                    ((v & 0x0000FFu) << 16);
	return v;
}

static u32 ra_chain_combine(u32 pv, u32 o, u8 op)
{
	if (op == RA_CN_OP_AND)        return pv & o;
	if (op == RA_CN_OP_ADD)        return pv + o;
	if (op == RA_CN_OP_SUB)        return pv - o;
	if (op == RA_CN_OP_MULT)       return pv * o;
	if (op == RA_CN_OP_DIV)        return o ? pv / o : 0;
	if (op == RA_CN_OP_XOR)        return pv ^ o;
	if (op == RA_CN_OP_MOD)        return o ? pv % o : 0;
	if (op == RA_CN_OP_SUB_PARENT) return o - pv;
	if (op == RA_CN_OP_ADD_ACC)    return pv + o;
	if (op == RA_CN_OP_SUB_ACC)    return pv - o;
	return 0;
}

/* GC MEM1 guard (24 MB, 0-based). A wild deref outside the window would
 * alias through ra_read_mem1's mask (or data-abort on a raw read), so an
 * out-of-range address makes the node INVALID instead — same policy as the
 * Wii walker, minus the MEM2 branch (GC games have no MEM2). */
static int ra_chain_addr_ok(u32 addr, u8 w)
{
	u32 end = addr + (u32)w - 1u;
	if (end < addr) return 0;                                 /* wrap */
	if (end <= 0x017FFFFFu) return 1;                         /* GC MEM1 */
	return 0;
}

/* Walk the whole table in slot order (parents always precede children).
 * Fills ra_chain_values[] + ra_chain_node_valid[]. Children of an invalid
 * node are invalid without reading.
 *
 * Phase D: an edge whose psize dp-kind != CUR reads the parent's
 * values_prev (DELTA) or prior (PRIOR) instead of values — the parent was
 * processed EARLIER this walk, so its values_prev already means "value at
 * walk N-1". History updates follow the rcheevos rule per node: prev = old,
 * prior = old only when the value changed. dp edges are invalid until the
 * second walk (no history yet); an out-of-range address computed from a
 * garbage prev is caught by the range check like any other. */
static void ra_chain_walk(void)
{
	u16 i;

	/* Snapshot last walk's validity BEFORE overwriting — pairs with the
	 * values_prev the dp section ships (both describe walk N-1). */
	memcpy(ra_chain_valid_prev, ra_chain_node_valid, sizeof(ra_chain_valid_prev));

	for (i = 0; i < ra_chain_node_count; i++) {
		const ra_chain_node_t *n = &ra_chain_nodes[i];
		u8  op    = n->op & 0x1F;
		u8  dpk   = (u8)(n->psize >> RA_CN_PSZ_DP_SHIFT);
		u8  valid = 1;
		u32 pv    = 0;
		u32 v     = 0;
		u32 old;

		if (n->parent != RA_CHAIN_PARENT_NONE) {
			if (!(ra_chain_node_valid[n->parent >> 3] & (1u << (n->parent & 7))))
				valid = 0;
			else if (dpk != RA_CN_DP_CUR && !ra_chain_have_prev)
				valid = 0;   /* no history yet — dp edge unresolvable */
			else {
				u32 praw = (dpk == RA_CN_DP_DELTA) ? ra_chain_values_prev[n->parent]
				         : (dpk == RA_CN_DP_PRIOR) ? ra_chain_prior[n->parent]
				         :                           ra_chain_values[n->parent];
				pv = ra_chain_xform(praw, (u8)(n->psize & RA_CN_PSZ_MEM_MASK));
			}
		}

		if (op == RA_CN_OP_NONE) {
			v = n->operand;
		}
		else if (op == RA_CN_OP_DEREF) {
			u32 addr = (n->parent == RA_CHAIN_PARENT_NONE) ? n->operand
			                                               : (pv + n->operand);
			u8 w = (u8)(((n->op >> 5) & 3) + 1);
			if (valid && ra_chain_addr_ok(addr, w)) {
				u8 k;
				for (k = 0; k < w; k++)
					v |= (u32)ra_read_mem1(addr + k) << (8u * k);
			} else {
				valid = 0;
				v = 0;
			}
		}
		else {
			if (valid)
				v = ra_chain_combine(pv, n->operand, op);
		}

		/* Frame history (Phase D): rcheevos update rule. */
		old = ra_chain_values[i];
		ra_chain_values_prev[i] = old;
		if (v != old)
			ra_chain_prior[i] = old;

		ra_chain_values[i] = v;
		if (valid)
			ra_chain_node_valid[i >> 3] |=  (u8)(1u << (i & 7));
		else
			ra_chain_node_valid[i >> 3] &= (u8)~(1u << (i & 7));
	}
	ra_chain_have_prev = 1;
}

/* Append helpers for debug strings. */
static u32 ra_apphex32(char *d, u32 v) { int i; for (i = 7; i >= 0; i--) d[7 - i] = ra_hexd((v >> (i * 4)) & 0xF); return 8; }
static u32 ra_appdec(char *d, u32 v)   { char t[10]; u32 n = 0, o = 0; if (!v) { d[0] = '0'; return 1; } while (v) { t[n++] = (char)('0' + v % 10); v /= 10; } while (n) d[o++] = t[--n]; return o; }
static u32 ra_apphex8(char *d, u32 v) __attribute__((unused));
static u32 ra_apphex8(char *d, u32 v)  { u32 i; for (i = 0; i < 8; i++) { u32 nib = (v >> ((7 - i) * 4)) & 0xF; d[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10); } return 8; }

/* Fetch one watchlist chunk via the legacy 2-transaction pattern — NOT Phase B
 * (matches the canonical d2x ra-module's GET_CHUNK): T1 asks the ESP to prepare
 * chunk N (the read is the stale previous response), wait ~50 ms, T2 reads the
 * now-prepared chunk. Appends to ra_watchlist[]. Returns 1=last, 0=more, <0=err. */
static int ra_fetch_watchlist_chunk(u16 chunk_index)
{
	struct __attribute__((packed)) {
		ra_gc_header_t           hdr;
		ra_watchlist_chunk_req_t req;
	} tx;
	ra_esp_header_t      resp;
	ra_watchlist_chunk_t chunk;
	const u32 PAD = 6;   /* ESP prepends 6 FF (GC_WRITE_PADDING). With the legacy
	                      * 2-tx those were eaten by T2's write phase; in Phase B
	                      * the write is a SEPARATE CS-low, so the pad lands at the
	                      * START of the read → the real response begins at +6. */
	u16 i;

	tx.hdr.magic       = RA_MAGIC_GC_TO_ESP;
	tx.hdr.command     = RA_CMD_GET_WATCHLIST_CHUNK;
	tx.hdr.payload_len = sizeof(ra_watchlist_chunk_req_t);
	tx.req.chunk_index = chunk_index;

	/* Phase B (INT handshake) instead of the legacy 2-tx timing path: robust at
	 * the high RA thread priority (the 2-tx outran the ESP's re-arm and read
	 * stale → glc m=00 → empty watchlist). The ESP already arms the INT on
	 * prepare_response for GET_CHUNK, so no ESP change is needed. */
	memset(ra_chunk_rx, 0, sizeof(ra_chunk_rx));
	if (ra_phase_b(&tx, sizeof(tx), ra_chunk_rx, sizeof(ra_chunk_rx)) < 0) return -1;

	memcpy(&resp,  ra_chunk_rx + PAD, sizeof(resp));
	memcpy(&chunk, ra_chunk_rx + PAD + sizeof(ra_esp_header_t), sizeof(chunk));
	{   /* DIAG: what did the chunk read actually return? */
		char m[64]; u32 o = 0; const char *p = "glc m=";
		while (*p) m[o++] = *p++;
		m[o++] = ra_hexd(resp.magic >> 4); m[o++] = ra_hexd(resp.magic & 0xF);
		p = " ac="; while (*p) m[o++] = *p++;
		o += ra_appdec(m + o, chunk.addr_count);
		p = " raw="; while (*p) m[o++] = *p++;
		{ u16 k; for (k = 0; k < 8; k++) { m[o++] = ra_hexd(ra_chunk_rx[PAD+k] >> 4); m[o++] = ra_hexd(ra_chunk_rx[PAD+k] & 0xF); } }
		m[o] = 0;
		ra_debug(m);
	}
	if (resp.magic != RA_MAGIC_ESP_TO_GC)            return -100;
	if (chunk.addr_count > RA_WATCHLIST_CHUNK_ADDRS) return -101;
	if (chunk.chunk_index != chunk_index)            return -102;
	{
		const u8 *src = ra_chunk_rx + PAD + sizeof(ra_esp_header_t) + sizeof(ra_watchlist_chunk_t);
		for (i = 0; i < chunk.addr_count && ra_watch_count < RA_MAX_WATCH; i++) {
			u32 a; memcpy(&a, src + i * 4, 4);
			ra_watchlist[ra_watch_count++] = a;
		}
	}
	return chunk.is_last ? 1 : 0;
}

/* Fetch all chunks until is_last. Returns 0 on success, negative on error. */
static int ra_fetch_watchlist(void)
{
	u16 idx = 0;
	int ret;
	ra_watch_count = 0;
	while (idx < 32) {
		ret = ra_fetch_watchlist_chunk(idx);
		if (ret < 0) return ret;
		if (ret == 1) return 0;
		idx++;
	}
	return -200;
}

/* Fetch one chain-table chunk (Phase C). Same Phase B + PAD=6 pattern as
 * ra_fetch_watchlist_chunk. *base = nodes accumulated so far. Returns 1 on
 * is_last, 0 for more, negative on error. */
static int ra_fetch_chain_chunk(u16 chunk_index, u16 *base)
{
	struct __attribute__((packed)) {
		ra_gc_header_t        hdr;
		ra_chain_chunk_req_t  req;
	} tx;
	ra_esp_header_t   resp;
	ra_chain_chunk_t  chunk;
	const u32 PAD = 6;   /* GC_WRITE_PADDING — see ra_fetch_watchlist_chunk */
	u16 i;
	int ret;

	tx.hdr.magic       = RA_MAGIC_GC_TO_ESP;
	tx.hdr.command     = RA_CMD_GET_CHAIN_CHUNK;
	tx.hdr.payload_len = sizeof(ra_chain_chunk_req_t);
	tx.req.chunk_index = chunk_index;

	memset(ra_chunk_rx, 0, sizeof(ra_chunk_rx));
	ret = ra_phase_b(&tx, sizeof(tx), ra_chunk_rx, sizeof(ra_chunk_rx));
	if (ret < 0) return ret;

	memcpy(&resp, ra_chunk_rx + PAD, sizeof(resp));
	if (resp.magic != RA_MAGIC_ESP_TO_GC) return -100;

	memcpy(&chunk, ra_chunk_rx + PAD + sizeof(ra_esp_header_t), sizeof(chunk));
	if (chunk.node_count > RA_CHAIN_CHUNK_NODES)           return -101;
	if (chunk.chunk_index != chunk_index)                  return -102;
	if (chunk.total_nodes > RA_GC_CHAIN_NODES)            return -103;
	if ((u32)*base + chunk.node_count > chunk.total_nodes) return -104;

	memcpy(&ra_chain_nodes[*base],
	       ra_chunk_rx + PAD + sizeof(ra_esp_header_t) + sizeof(ra_chain_chunk_t),
	       (u32)chunk.node_count * RA_CHAIN_NODE_SIZE);

	/* Structural validation: parents must reference EARLIER slots (the
	 * walker is a single forward pass) and ops must be known. A violation
	 * poisons the whole table -> caller disables Phase C. */
	for (i = 0; i < chunk.node_count; i++) {
		const ra_chain_node_t *n = &ra_chain_nodes[*base + i];
		u8 op = n->op & 0x1F;
		if (n->parent != RA_CHAIN_PARENT_NONE && n->parent >= (u16)(*base + i))
			return -105;
		if (op != RA_CN_OP_NONE && op != RA_CN_OP_DEREF &&
		    (op < RA_CN_OP_MULT || op > RA_CN_OP_SUB_ACC))
			return -106;
	}

	*base = (u16)(*base + chunk.node_count);
	return chunk.is_last ? 1 : 0;
}

/* Fetch the whole chain table. Any failure => Phase C off (legacy-only);
 * an ESP without a table answers node_count=0/is_last=1 on chunk 0. The
 * ESP arms its Phase-C coverage only when the LAST chunk is fetched, so
 * an aborted fetch leaves BOTH sides in pure-legacy — never split-brain. */
static void ra_fetch_chain_table(void)
{
	u16 base = 0;
	u16 chunk_idx = 0;
	u16 i;
	int ret;
	char m[48];
	u32 o;

	ra_chain_node_count = 0;
	ra_chain_shipped    = 0;
	ra_chain_win_next   = 0;

	while (chunk_idx < (RA_GC_CHAIN_NODES / RA_CHAIN_CHUNK_NODES) + 1) {
		ret = ra_fetch_chain_chunk(chunk_idx, &base);
		if (ret < 0) {
			o = 0;
			const char *p = "GC-RA CHN err=-";
			while (*p) m[o++] = *p++;
			o += ra_appdec(m + o, (u32)(-ret));
			m[o] = 0;
			ra_debug(m);
			return;   /* legacy-only */
		}
		if (ret == 1) break;
		chunk_idx++;
	}

	ra_chain_node_count = base;
	for (i = 0; i < ra_chain_node_count; i++) {
		if (ra_chain_nodes[i].op & 0x80)
			ra_chain_ship_node[ra_chain_shipped++] = i;
	}

	/* Phase D: reset per-node history + build the dp verification list —
	 * distinct (parent,kind) pairs in table-scan order (the ESP derives the
	 * identical list from its copy of the table). Emitter caps the count at
	 * RA_MAX_DP_PARENTS; overflow here means a corrupt table -> reject. */
	ra_chain_have_prev = 0;
	ra_chain_dp_count  = 0;
	for (i = 0; i < ra_chain_node_count; i++) {
		ra_chain_values[i]      = 0;
		ra_chain_values_prev[i] = 0;
		ra_chain_prior[i]       = 0;
	}
	memset(ra_chain_node_valid, 0, sizeof(ra_chain_node_valid));
	memset(ra_chain_valid_prev, 0, sizeof(ra_chain_valid_prev));
	for (i = 0; i < ra_chain_node_count; i++) {
		const ra_chain_node_t *n = &ra_chain_nodes[i];
		u8 dpk = (u8)(n->psize >> RA_CN_PSZ_DP_SHIFT);
		u16 j;
		if (dpk == RA_CN_DP_CUR || n->parent == RA_CHAIN_PARENT_NONE)
			continue;
		for (j = 0; j < ra_chain_dp_count; j++)
			if (ra_chain_dp_node[j] == n->parent && ra_chain_dp_kind[j] == dpk)
				break;
		if (j < ra_chain_dp_count)
			continue;
		if (ra_chain_dp_count >= RA_MAX_DP_PARENTS) {
			ra_chain_node_count = 0;   /* corrupt table — Phase C off */
			ra_chain_shipped    = 0;
			ra_chain_dp_count   = 0;
			return;
		}
		ra_chain_dp_node[ra_chain_dp_count] = n->parent;
		ra_chain_dp_kind[ra_chain_dp_count] = dpk;
		ra_chain_dp_count++;
	}

	{
		const char *p = "GC-RA CHN n=";
		o = 0;
		while (*p) m[o++] = *p++;
		o += ra_appdec(m + o, ra_chain_node_count);
		m[o++]=' ';m[o++]='s';m[o++]='h';m[o++]='=';
		o += ra_appdec(m + o, ra_chain_shipped);
		m[o++]=' ';m[o++]='d';m[o++]='p';m[o++]='=';
		o += ra_appdec(m + o, ra_chain_dp_count);
		m[o] = 0;
		ra_debug(m);
	}
}

/* Central response parser (Phase D2 — faithful port of the canonical d2x
 * ra-module). Mutates ra_pending_q*, ra_watchlist*, ra_wl_seq, ra_resync_*.
 *   ADDR_QUERY        — store pending query AND append the same addrs to the
 *                       permanent watchlist (UNIFY v0.30: ESP appends the same
 *                       set on ADDR_RESPONSE, so both replicas grow identically).
 *   WATCHLIST_APPEND  — apply iff seq == ra_wl_seq+1 (idempotent delivery).
 *   WATCHLIST_REMOVE_IDX — index-based compaction, apply iff seq == ra_wl_seq+1.
 *   WATCHLIST_UPDATE  — full re-fetch requested (defer to main loop).
 *   ACHIEVEMENT       — flag for the loop to celebrate + log. */
static void ra_parse_response(const u8 *buf)
{
	ra_esp_header_t resp;
	memcpy(&resp, buf, sizeof(resp));
	if (resp.magic != RA_MAGIC_ESP_TO_GC) return;

	if (resp.event_type == RA_EVT_ADDR_QUERY) {
		ra_addr_query_t aq;
		memcpy(&aq, buf + sizeof(resp), sizeof(aq));
		if (aq.addr_count > 0 && aq.addr_count <= RA_ADDR_QUERY_MAX) {
			const u8 *src = buf + sizeof(resp) + sizeof(aq);
			u16 j;
			for (j = 0; j < aq.addr_count; j++)
				memcpy(&ra_pending_q[j], src + j * 4, 4);
			ra_pending_qn = aq.addr_count;
			/* UNIFY: append the queried addrs to the watchlist now (the ESP
			 * does the same on ADDR_RESPONSE — kept in lockstep). */
			if ((u32)ra_watch_count + aq.addr_count <= RA_MAX_WATCH) {
				for (j = 0; j < aq.addr_count; j++)
					ra_watchlist[ra_watch_count + j] = ra_pending_q[j];
				ra_watch_count += aq.addr_count;
			}
		}
	} else if (resp.event_type == RA_EVT_WATCHLIST_APPEND) {
		ra_watchlist_append_t wa;
		memcpy(&wa, buf + sizeof(resp), sizeof(wa));
		if (wa.seq == (u16)(ra_wl_seq + 1)
		    && wa.addr_count > 0
		    && (u32)ra_watch_count + wa.addr_count <= RA_MAX_WATCH) {
			const u8 *src = buf + sizeof(resp) + sizeof(wa);
			u16 j;
			for (j = 0; j < wa.addr_count; j++)
				memcpy(&ra_watchlist[ra_watch_count + j], src + j * 4, 4);
			ra_watch_count += wa.addr_count;
			ra_wl_seq = wa.seq;
		}
	} else if (resp.event_type == RA_EVT_WATCHLIST_REMOVE_IDX) {
		ra_watchlist_remove_idx_t wri;
		memcpy(&wri, buf + sizeof(resp), sizeof(wri));
		if (wri.seq == (u16)(ra_wl_seq + 1)
		    && wri.idx_count > 0 && wri.idx_count <= ra_watch_count) {
			const u8 *src = buf + sizeof(resp) + sizeof(wri);
			u16 i, skip = 0, w = 0;
			for (i = 0; i < ra_watch_count; i++) {
				if (skip < wri.idx_count) {
					u16 idx; memcpy(&idx, src + skip * 2, 2);
					if (idx == i) { skip++; continue; }
				}
				if (w != i) ra_watchlist[w] = ra_watchlist[i];
				w++;
			}
			ra_watch_count = w;
			ra_wl_seq = wri.seq;
		}
	} else if (resp.event_type == RA_EVT_WATCHLIST_UPDATE) {
		ra_watchlist_notify_t wn;
		memcpy(&wn, buf + sizeof(resp), sizeof(wn));
		ra_resync_seq = wn.seq;
		ra_resync_pending = 1;
	} else if (resp.event_type == RA_EVT_ACHIEVEMENT) {
		ra_ach_fired = 1;
	}
}

/* DIAGNOSTIC knobs (2026-06-14): the full 547-entry snapshot (559 B write +
 * 2080 B read) crashes the running GC game, while n=0 was stable for 16 min.
 * Shrink the transaction to test whether transaction SIZE is the cause:
 *   RA_SNAP_TEST_CAP  — cap addresses sent per snapshot (0 = no cap).
 *   RA_SNAP_TEST_READ — bytes to read back per snapshot (small = tiny txn).
 * If the game is STABLE with tiny values, the crash is transaction size and we
 * chunk the protocol; if it still crashes, it's something more fundamental. */
#define RA_SNAP_TEST_CAP   0    /* 0 = full watchlist (diagnostic isolation off) */
#define RA_SNAP_TEST_READ  0    /* 0 = full RA_RESP_LEN read */

/* Read all watchlist values from MEM1 and ship a SNAPSHOT (v3 header:
 * flat values + Phase C chain window bitmap/blob + Phase D dp sections —
 * faithful port of the canonical d2x ra_send_snapshot). With no chain
 * table (fetch failed / ESP has none) all the v3 sections are empty and
 * this degrades to the legacy flat snapshot. */
static int ra_send_snapshot(void)
{
	ra_gc_header_t       *hdr  = (ra_gc_header_t *)ra_snap_tx;
	ra_snapshot_header_t *snap = (ra_snapshot_header_t *)(ra_snap_tx + sizeof(ra_gc_header_t));
	u16 count = ra_watch_count;
	u8 *vals  = ra_snap_tx + sizeof(ra_gc_header_t) + sizeof(ra_snapshot_header_t);
	u32 tx_len, rx_len;
	u16 i;

	u16 win_first = 0, win_count = 0;
	u32 blob_len = 0, bm_bytes = 0;

#if RA_SNAP_TEST_CAP
	if (count > RA_SNAP_TEST_CAP) count = RA_SNAP_TEST_CAP;
#endif

	hdr->magic       = RA_MAGIC_GC_TO_ESP;
	hdr->command     = RA_CMD_SNAPSHOT;
	++ra_frame_counter;                          /* delivery count */
	snap->frame_counter = ra_game_frame;         /* real-time 60 Hz frame clock */
	snap->addr_count    = count;
	snap->wl_seq        = ra_wl_seq;             /* Phase D2 sync echo */
	for (i = 0; i < count; i++)
		vals[i] = ra_read_mem1(ra_watchlist[i]);
	if (ra_chain_node_count > 0)
		ra_chain_walk();

	/* Phase C chain window — greedy fill from the rotating cursor until the
	 * 8192B transaction budget is hit; next frame resumes where we stopped
	 * (wraps to 0 at the end). Bitmap bit i = window slot i valid.
	 * Phase D: the dp verification section (4B per dp entry) is reserved
	 * OFF THE TOP so the window can never squeeze it out. */
	if (ra_chain_node_count > 0 && ra_chain_shipped > 0) {
		u32 avail = sizeof(ra_snap_tx) - sizeof(ra_gc_header_t)
		          - sizeof(ra_snapshot_header_t) - count
		          - (u32)ra_chain_dp_count * 4u
		          - ((u32)ra_chain_dp_count + 7u) / 8u;  /* dp validity bitmap */
		u16 s = (ra_chain_win_next < ra_chain_shipped) ? ra_chain_win_next : 0;

		win_first = s;
		while (s < ra_chain_shipped) {
			const ra_chain_node_t *n = &ra_chain_nodes[ra_chain_ship_node[s]];
			u8  w  = (u8)(((n->op >> 5) & 3) + 1);
			u32 nb = (((u32)win_count + 1u) + 7u) / 8u;
			if (nb + blob_len + w > avail) break;
			blob_len += w;
			win_count++;
			s++;
		}
		bm_bytes = ((u32)win_count + 7u) / 8u;
		ra_chain_win_next = (s >= ra_chain_shipped) ? 0 : s;

		{
			u8 *bm   = vals + count;
			u8 *blob = bm + bm_bytes;
			u16 wi;
			for (wi = 0; wi < bm_bytes; wi++)
				bm[wi] = 0;
			for (wi = 0; wi < win_count; wi++) {
				u16 node = ra_chain_ship_node[win_first + wi];
				const ra_chain_node_t *n = &ra_chain_nodes[node];
				u8  w = (u8)(((n->op >> 5) & 3) + 1);
				u32 v = ra_chain_values[node];
				u8  k;
				if (ra_chain_node_valid[node >> 3] & (1u << (node & 7)))
					bm[wi >> 3] |= (u8)(1u << (wi & 7));
				for (k = 0; k < w; k++)
					blob[k] = (u8)(v >> (8u * k));
				blob += w;
			}
		}
	}
	snap->chain_first    = win_first;
	snap->chain_count    = win_count;
	snap->chain_blob_len = (u16)blob_len;
	snap->dp_count       = ra_chain_dp_count;

	/* Phase D dp verification section — the prev/prior parent values the
	 * walker used THIS frame (list order fixed at fetch; the ESP compares
	 * against its own memref delta/prior after its upd and defers on
	 * mismatch). Written after the window blob. */
	if (ra_chain_dp_count > 0) {
		u8 *dp = vals + count + bm_bytes + blob_len;
		u8 *dpv = dp + (u32)ra_chain_dp_count * 4u;   /* validity bitmap */
		u32 dpv_bytes = ((u32)ra_chain_dp_count + 7u) / 8u;
		u16 di;
		for (di = 0; di < dpv_bytes; di++)
			dpv[di] = 0;
		for (di = 0; di < ra_chain_dp_count; di++) {
			u16 node = ra_chain_dp_node[di];
			u32 dv = (ra_chain_dp_kind[di] == RA_CN_DP_PRIOR)
			         ? ra_chain_prior[node]
			         : ra_chain_values_prev[node];
			memcpy(dp + (u32)di * 4u, &dv, 4);   /* Starlet is BE = wire order */
			/* validity of the shipped value = the parent RESOLVED at walk
			 * N-1 (values_prev meaningful). ESP skips the compare on 0. */
			if (ra_chain_valid_prev[node >> 3] & (1u << (node & 7)))
				dpv[di >> 3] |= (u8)(1u << (di & 7));
		}
	}

	hdr->payload_len = (u16)(sizeof(ra_snapshot_header_t) + count
	                         + bm_bytes + blob_len
	                         + (u32)ra_chain_dp_count * 4u
	                         + ((u32)ra_chain_dp_count + 7u) / 8u);
	tx_len = sizeof(ra_gc_header_t) + sizeof(ra_snapshot_header_t) + count
	       + bm_bytes + blob_len + (u32)ra_chain_dp_count * 4u
	       + ((u32)ra_chain_dp_count + 7u) / 8u;
	rx_len = RA_SNAP_TEST_READ ? RA_SNAP_TEST_READ : RA_RESP_LEN;
	memset(ra_snap_rx, 0, sizeof(ra_snap_rx));
	if (ra_phase_b(ra_snap_tx, tx_len, ra_snap_rx, rx_len) < 0) return -1;
	ra_parse_response(ra_snap_rx);
	return 0;
}

/* Answer a pending ADDR_QUERY by reading the requested addrs from MEM1. */
static int ra_send_addr_response(void)
{
	ra_gc_header_t     *hdr = (ra_gc_header_t *)ra_snap_tx;
	ra_addr_response_t *ar  = (ra_addr_response_t *)(ra_snap_tx + sizeof(ra_gc_header_t));
	u16 count = ra_pending_qn;
	u8 *vals  = ra_snap_tx + sizeof(ra_gc_header_t) + sizeof(ra_addr_response_t);
	u32 tx_len;
	u16 i;

	if (count == 0) return 0;
	hdr->magic       = RA_MAGIC_GC_TO_ESP;
	hdr->command     = RA_CMD_ADDR_RESPONSE;
	hdr->payload_len = sizeof(ra_addr_response_t) + count;
	ar->addr_count   = count;
	for (i = 0; i < count; i++)
		vals[i] = ra_read_mem1(ra_pending_q[i]);

	tx_len = sizeof(ra_gc_header_t) + sizeof(ra_addr_response_t) + count;
	memset(ra_snap_rx, 0, sizeof(ra_snap_rx));
	if (ra_phase_b(ra_snap_tx, tx_len, ra_snap_rx, RA_RESP_LEN) < 0) return -1;
	ra_pending_qn = 0;
	ra_parse_response(ra_snap_rx);
	return 0;
}

/* ------------------------------------------------------------------------
 * Background thread — post-boot watchlist fetch + SNAPSHOT loop.
 * (The pre-boot handshake already drove LOAD_GAME -> GAME_LOADED.)
 * ------------------------------------------------------------------------ */
static u32 RA_PollThread(void *arg)
{
	int i;
	char m[80];
	u32 o;
	(void)arg;

	led_setup();
	for (i = 0; i < 3; i++) { led_on(); mdelay(150); led_off(); mdelay(150); }

	/* Reserve the disc-slot LED exclusively for achievement notifications:
	 * disable Nintendont's SD/USB access indicator so the LED stays DARK during
	 * play and only lights when led_celebrate() flashes on an unlock (like the
	 * Wii). Without this the constant game streaming holds the LED on. */
	access_led = false;
	led_off();

	/* Let the game boot before we start (no VBI game-alive signal yet). */
	mdelay(5000);

	/* Pull the watchlist (ESP is GAME_LOADED from the pre-boot handshake) via
	 * Phase B GET_CHUNK. Retry a few times for safety (ra_fetch_watchlist resets
	 * the count each call, so retries are clean). No IDENTIFY prime needed — the
	 * legacy 2-tx required it; Phase B's INT handshake does not. */
	{
		int wr = -1, attempt;
		for (attempt = 0; attempt < 5; attempt++) {
			wr = ra_fetch_watchlist();
			if (wr == 0) break;
			mdelay(100);   /* let the ESP re-arm before retrying */
		}
		if (wr == 0) {
			o = 0;
			m[o++]='w';m[o++]='l';m[o++]=' ';m[o++]='n';m[o++]='=';
			o += ra_appdec(m + o, ra_watch_count);
			for (i = 0; i < 3 && i < ra_watch_count; i++) {
				m[o++]=' ';m[o++]='a';m[o++]=(char)('0'+i);m[o++]='=';
				o += ra_apphex32(m + o, ra_watchlist[i]);
			}
			m[o] = 0;
			ra_debug(m);
			/* Phase C: fetch the chain descriptor table (once per game).
			 * Failure => ra_chain_node_count stays 0 => pure legacy, and
			 * the ESP (which arms coverage only on the LAST chunk) stays
			 * legacy too — never split-brain. */
			ra_fetch_chain_table();
		} else {
			o = 0;
			m[o++]='w';m[o++]='l';m[o++]=' ';m[o++]='F';m[o++]='A';m[o++]='I';m[o++]='L';m[o++]=' ';
			m[o++]='r';m[o++]='c';m[o++]='=';
			if (wr < 0) { m[o++]='-'; o += ra_appdec(m + o, (u32)(-wr)); }
			m[o] = 0;
			ra_debug(m);
		}
	}

	/* (No mid-run priority raise — IOS rejects raising own priority; the thread
	 * is created at RA_THREAD_PRIO=0x78 and GET_CHUNK is Phase B, so init is safe
	 * at high priority.) */

	/* SNAPSHOT loop — timer-paced ~30 Hz for now (VBlank sync comes next).
	 * Drain up to 12 ADDR_QUERY rounds per frame (multi-pass convergence).
	 * Handle Phase D2 in-game RESYNC (full re-fetch) and achievement events. */
	{
		u32 frames = 0;
		u32 next_fire;
		ra_start_tick = read32(HW_TIMER);
		next_fire     = ra_start_tick + RA_FRAME_TICKS;
		for (;;) {
			/* Frame clock. Baseline: 60 Hz HW_TIMER wall-clock (u32 subtraction
			 * wraps correctly within the timer's ~38-min period). */
			u32 hw_frame = (read32(HW_TIMER) - ra_start_tick) / RA_FRAME_TICKS;
#if RA_USE_VBI
			{
				u32 vbi = ra_read_vbi();
				if (!ra_vbi_live) {
					/* Trust the PPC counter only once we've seen it advance. */
					if (ra_vbi_last == 0)        ra_vbi_last = vbi;
					else if (vbi != ra_vbi_last) {
						ra_vbi_live = 1;
						ra_vbi_base = vbi;   /* zero our frame count at go-live */
						ra_debug("GC-RA VBI counter live (true game frames)");
					}
				}
				/* FRAME-ALIGN (Dolphin-like, toggle RA_FRAME_ALIGN): once the VBI
				 * clock is live, wait for the NEXT frame tick before sampling MEM1
				 * so every snapshot is a settled frame-boundary state. Costs up to
				 * ~1 frame/snapshot (big df/s hit + erratic on Melee) — default OFF. */
#if RA_FRAME_ALIGN
				if (ra_vbi_live) {
					u32 t0 = read32(HW_TIMER);
					while (ra_read_vbi() == vbi) {
						if ((read32(HW_TIMER) - t0) > RA_FRAME_TICKS * 3u) break;
						udelay(200);
					}
					vbi = ra_read_vbi();
				}
#endif
				ra_game_frame = ra_vbi_live ? (vbi - ra_vbi_base) : hw_frame;
				ra_vbi_last = vbi;
			}
#else
			ra_game_frame = hw_frame;
#endif

			(void)ra_send_snapshot();
#if !RA_SNAP_TEST_CAP
			for (i = 0; i < 12 && ra_pending_qn > 0; i++)
				(void)ra_send_addr_response();

			/* Phase D2 RESYNC: the ESP published a fresh full list. Re-fetch
			 * the watchlist and adopt the notify's seq base. */
			if (ra_resync_pending) {
				ra_resync_pending = 0;
				if (ra_fetch_watchlist() == 0) {
					ra_wl_seq = ra_resync_seq;
					ra_debug("GC-RA RESYNC: watchlist re-fetched");
				}
			}
#else
			/* DIAGNOSTIC mode: tiny snapshot only — no ADDR_RESPONSE, no resync
			 * churn — so the ONLY RA EXI activity is the small capped snapshot. */
			ra_pending_qn = 0;
			ra_resync_pending = 0;
#endif

			/* Keep the disc-slot LED exclusively ours: force Nintendont's
			 * disk-access indicator off so it can't drive the slot LED (it would
			 * fight our off/blink). The pin is set up as OUTPUT once at thread
			 * start (led_setup with DIR=1 — the fix for the long-standing
			 * "LED stuck on" bug; the pin had been left as an input). */
			access_led = false;

			/* Achievement unlocked — kick off the (non-blocking) celebration. */
			if (ra_ach_fired) {
				ra_ach_fired = 0;
				ra_celeb = RA_CELEB_FRAMES;
				ra_debug("GC-RA *** ACHIEVEMENT UNLOCKED ***");
			}
			/* Celebrate while ra_celeb > 0: 3 pulses (on for RA_BLINK_HALF,
			 * off the next, ...). The LED and the trophy badge blink together
			 * (the codehandler draws the badge whenever the flag is on). */
			if (ra_celeb > 0) {
				u32 on = !((ra_celeb / RA_BLINK_HALF) & 1u);
				if (on) led_on(); else led_off();
#if RA_USE_OVERLAY
				ra_set_trophy(on);
#endif
#if RA_USE_RUMBLE
				ra_set_rumble(ra_celeb > RA_CELEB_FRAMES - RA_RUMBLE_FRAMES);
#endif
				if (--ra_celeb == 0) {
					led_off();
#if RA_USE_OVERLAY
					ra_set_trophy(0);
#endif
#if RA_USE_RUMBLE
					ra_set_rumble(0);
#endif
				}
			} else {
				led_off();   /* normal gameplay: LED off */
			}

			/* Heartbeat once a second: alive + count + applied seq. */
			if (++frames >= 30) {
				frames = 0;
				o = 0;
				m[o++]='s';m[o++]='n';m[o++]='a';m[o++]='p';m[o++]=' ';
				m[o++]='f';m[o++]='=';
				o += ra_appdec(m + o, ra_frame_counter);
				m[o++]=' ';m[o++]='n';m[o++]='=';
				o += ra_appdec(m + o, ra_watch_count);
				m[o++]=' ';m[o++]='s';m[o++]='q';m[o++]='=';
				o += ra_appdec(m + o, ra_wl_seq);
				m[o++]=' ';m[o++]='g';m[o++]='f';m[o++]='=';
				o += ra_appdec(m + o, ra_game_frame);
				/* Phase C visibility: shipped chain slots (0 = legacy). */
				m[o++]=' ';m[o++]='c';m[o++]='h';m[o++]='=';
				o += ra_appdec(m + o, ra_chain_shipped);
#if RA_USE_VBI
				/* VBI diagnostics: raw PPC counter + live flag. When live, gf
				 * tracks vb-base; compare vb's rate to gf to confirm ~60 Hz. */
				m[o++]=' ';m[o++]='v';m[o++]='b';m[o++]='=';
				o += ra_appdec(m + o, ra_vbi_last);
				m[o++]=' ';m[o++]='L';m[o++]='v';m[o++]='=';
				o += ra_appdec(m + o, (u32)ra_vbi_live);
				/* OVER-COUNT PROBE: pc = codehandler calls via the PADRead path
				 * (0x1018). Compare Δpc to Δvb (= total, 0x1004) between heartbeats:
				 * Δpc==Δvb => game polls PADRead Nx/frame; Δvb>Δpc => OSSleepThread
				 * /PADHook stubs add the rest. */
				m[o++]=' ';m[o++]='p';m[o++]='c';m[o++]='=';
				sync_before_read((void*)0x1000, 0x20);
				o += ra_appdec(m + o, read32(0x1018u));
#endif
#if RA_USE_OVERLAY
				/* Trophy diagnostics: tf = raw VI_TFBL the codehandler reads
				 * (published @0x1010 every frame); bx = badge-exec count (@0x1014,
				 * bumped each time the badge actually drew → flag seen + TFBL ok). */
				sync_before_read((void*)0x1000, 0x20);
				m[o++]=' ';m[o++]='t';m[o++]='f';m[o++]='=';
				o += ra_apphex8(m + o, read32(0x1010u));
				m[o++]=' ';m[o++]='b';m[o++]='x';m[o++]='=';
				o += ra_appdec(m + o, read32(0x1014u));
#endif
				m[o] = 0;
				ra_debug(m);
			}

			/* Pacing. Only when frame-align is ON *and* the VBI clock is live does
			 * the gate above pace us — skip the timer then. Otherwise (frame-align
			 * off, or no VBI) use the HW_TIMER pacing (which adds no delay anyway
			 * once per-snapshot work overruns 60 Hz). */
#if RA_FRAME_ALIGN
			if (!ra_vbi_live)
#endif
			{
				u32 now = read32(HW_TIMER);
				s32 togo = (s32)(next_fire - now);
				if (togo > 0) {
					udelay((int)(((u32)togo * 527u) / 1000u));
					next_fire += RA_FRAME_TICKS;
				} else if (-togo > (s32)RA_FRAME_TICKS) {
					next_fire = now + RA_FRAME_TICKS;
				} else {
					next_fire += RA_FRAME_TICKS;
				}
			}
		}
	}

	return 0;
}

/* ------------------------------------------------------------------------
 * Game-image reader (pre-boot safe).
 *
 * Reads `len` bytes at byte `offset` from the game image into `dst`, using the
 * same backend DIReadThread uses (ReadRealDisc / ISORead / FSTRead). Disc
 * reads pass NeedSync=false so ReadRealDisc polls DIP_CONTROL directly instead
 * of waiting on RealDI_Update() — which is only serviced from the kernel main
 * loop, not yet running at the pre-boot point. RealDI_Identify() already reads
 * offset 0 this exact way during init, so it is proven safe pre-boot.
 *
 * Returns 0 on success, negative on error. (FST not yet supported here: its
 * GamePath is file-static in DI.c — handled in a later step.)
 * ------------------------------------------------------------------------ */
static char ra_hexd(u8 n) { return (n < 10) ? (char)('0' + n) : (char)('A' + n - 10); }

static int ra_disc_read(u8 *dst, u32 offset, u32 len)
{
	while (len > 0) {
		u32 chunk = len;
		const u8 *src;
		if (RealDiscCMD)   src = ReadRealDisc(&chunk, offset, false);
		else if (FSTMode)  return -2;               /* TODO: FST GamePath is static */
		else               src = ISORead(&chunk, offset);
		if (!src || chunk == 0) return -1;
		if (chunk > len) chunk = len;
		memcpy(dst, src, chunk);
		dst += chunk; offset += chunk; len -= chunk;
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * GameCube RetroAchievements hash — faithful port of rcheevos
 * rc_hash_gamecube() / rc_hash_nintendo_disc_partition() with part_offset=0
 * and wii_shift=0. Streams data through MD5 in 32 KB chunks so we never need
 * to buffer the whole header/DOL. Writes 32 lowercase hex chars + NUL.
 *
 * Replicates rcheevos BYTE-FOR-BYTE — including the DOL segment seek using
 * part_offset + dol_offsets[ix] (NOT + dol_offset) — because the RA hash
 * database is built by that exact code; matching it is what lets the server
 * resolve the game. (Same principle as the Wii ra_hash.c quirk replication.)
 * Returns 0 on success, negative on error.
 * ------------------------------------------------------------------------ */
static u8 ra_hash_buf[0x8000] __attribute__((aligned(32)));   /* 32 KB stream chunk */

static u32 ra_be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/* The GameCube disc interface (ReadRealDisc, NORMAL command) requires the
 * read offset AND length to be 32-byte aligned — it passes them straight to
 * the DI hardware. Unaligned reads return garbage (confirmed: a 4-byte read
 * at 0x1C failed while the same bytes inside a 32-byte read at 0 were correct).
 * Both helpers below over-read to 32-byte-aligned bounds and slice locally. */
#define RA_ALN 0x20u

/* Stream-MD5 the exact byte range [offset, offset+len), reading the disc only
 * in 32-byte-aligned chunks and md5_appending just the requested bytes. */
static int ra_md5_range(md5_state_t *md5, u32 offset, u32 len)
{
	u32 cur  = offset & ~(RA_ALN - 1);
	u32 skip = offset - cur;                 /* skipped only in first chunk */
	while (len > 0) {
		u32 want  = skip + len;              /* useful bytes from cur onward */
		u32 aread = (want > sizeof(ra_hash_buf))
		                ? (u32)sizeof(ra_hash_buf)
		                : ((want + (RA_ALN - 1)) & ~(RA_ALN - 1));
		u32 avail, use;
		if (ra_disc_read(ra_hash_buf, cur, aread) < 0) return -1;
		avail = aread - skip;
		use   = (avail > len) ? len : avail;
		md5_append(md5, ra_hash_buf + skip, (int)use);
		len  -= use;
		cur  += aread;
		skip  = 0;
	}
	return 0;
}

/* Aligned small read: copy exact [offset, offset+len) into dst (len small). */
static int ra_read_small(u8 *dst, u32 offset, u32 len)
{
	static u8 b[256] __attribute__((aligned(32)));
	u32 cur   = offset & ~(RA_ALN - 1);
	u32 skip  = offset - cur;
	u32 aread = (skip + len + (RA_ALN - 1)) & ~(RA_ALN - 1);
	if (aread > sizeof(b)) return -1;
	if (ra_disc_read(b, cur, aread) < 0) return -1;
	memcpy(dst, b + skip, len);
	return 0;
}

static int ra_compute_gc_hash(char out_hex[33])
{
	static u8  dolhdr[0xD8];
	static u32 dol_offsets[18], dol_sizes[18];
	static const char hx[] = "0123456789abcdef";
	md5_state_t md5;
	u8  q[8], digest[16];
	u32 apploader_body, apploader_trailer, header_size, dol_offset;
	int ix;

	out_hex[0] = 0;

	/* GameCube magic word at 0x1C must be C2339F3D. */
	if (ra_read_small(q, 0x1C, 4) < 0) return -1;
	if (!(q[0] == 0xC2 && q[1] == 0x33 && q[2] == 0x9F && q[3] == 0x3D)) return -2;

	md5_init(&md5);

	/* Apploader sizes: body @ 0x2440+0x14, trailer @ +4. header_size =
	 * 0x2440 + 0x20 (apploader header) + body + trailer, capped at 1 MB. */
	if (ra_read_small(q, 0x2440 + 0x14, 8) < 0) return -1;
	apploader_body    = ra_be32(q);
	apploader_trailer = ra_be32(q + 4);
	header_size = 0x2440 + 0x20 + apploader_body + apploader_trailer;
	if (header_size > 1024u * 1024u) header_size = 1024u * 1024u;

	/* Hash the partition header [0, header_size). */
	if (ra_md5_range(&md5, 0, header_size) < 0) return -1;

	/* boot.dol offset @ 0x420. */
	if (ra_read_small(q, 0x420, 4) < 0) return -1;
	dol_offset = ra_be32(q);

	/* DOL segment table: 0xD8 bytes at dol_offset (7 code + 11 data). */
	if (ra_read_small(dolhdr, dol_offset, 0xD8) < 0) return -1;
	for (ix = 0; ix < 18; ix++) {
		dol_offsets[ix] = ra_be32(dolhdr + ix * 4);
		dol_sizes[ix]   = ra_be32(dolhdr + 0x90 + ix * 4);
	}

	/* Hash each non-empty segment. Seek = part_offset(0) + dol_offsets[ix]. */
	for (ix = 0; ix < 18; ix++) {
		if (dol_sizes[ix] == 0) continue;
		if (ra_md5_range(&md5, dol_offsets[ix], dol_sizes[ix]) < 0) return -1;
	}

	md5_finish(&md5, digest);
	for (ix = 0; ix < 16; ix++) {
		out_hex[ix * 2]     = hx[digest[ix] >> 4];
		out_hex[ix * 2 + 1] = hx[digest[ix] & 0xF];
	}
	out_hex[32] = 0;
	return 0;
}

/* ------------------------------------------------------------------------
 * LOAD_GAME + poll for GAME_LOADED. Mirrors WiiFlow ra_exi.c: the response in
 * the same (half-duplex) transaction is unreliable, so we send LOAD_GAME and
 * then poll RA_CMD_POLL until the ESP reports GAME_LOADED/ACTIVE (it fetches
 * the achievement set from the RA server, which can take a while). The ESP
 * prints its own progress on Serial; we just wait for the terminal status.
 *
 * Returns 0 on GAME_LOADED/ACTIVE, -2 on ESP error status, -1 on timeout.
 * ------------------------------------------------------------------------ */
#define RA_LOADGAME_POLLS    300   /* × 300 ms ≈ 90 s */
#define RA_LOADGAME_POLL_MS  300

static int ra_load_game(const char *game_id6, const char *md5_hex)
{
	struct __attribute__((packed)) {
		ra_gc_header_t hdr;
		char           game_id[RA_GAME_ID_LEN];
		u8             disc_number;
		u8             has_hash;
		char           md5_hash[RA_HASH_LEN];
	} tx;
	u8  rx[sizeof(ra_esp_header_t)];
	int attempt;

	tx.hdr.magic       = RA_MAGIC_GC_TO_ESP;
	tx.hdr.command     = RA_CMD_LOAD_GAME;
	tx.hdr.payload_len = (u16)(RA_GAME_ID_LEN + 1 + 1 + RA_HASH_LEN);
	memcpy(tx.game_id, game_id6, RA_GAME_ID_LEN);
	tx.disc_number = 0;
	if (md5_hex && md5_hex[0]) {
		tx.has_hash = 1;
		memcpy(tx.md5_hash, md5_hex, RA_HASH_LEN);
	} else {
		tx.has_hash = 0;
		memset(tx.md5_hash, 0, RA_HASH_LEN);
	}

	/* Send LOAD_GAME (ignore the unreliable same-transaction response). */
	ra_xfer(&tx, sizeof(tx), rx, sizeof(rx));

	/* Poll until terminal status. */
	for (attempt = 0; attempt < RA_LOADGAME_POLLS; attempt++) {
		ra_gc_header_t poll;
		const ra_esp_header_t *r;

		mdelay(RA_LOADGAME_POLL_MS);

		poll.magic       = RA_MAGIC_GC_TO_ESP;
		poll.command     = RA_CMD_POLL;
		poll.payload_len = 0;
		memset(rx, 0, sizeof(rx));
		if (ra_xfer(&poll, sizeof(poll), rx, sizeof(rx)) != 0)
			continue;

		r = (const ra_esp_header_t *)rx;
		if (r->magic != RA_MAGIC_ESP_TO_GC)
			continue;                       /* half-duplex idle FF — keep polling */
		if (r->status == RA_STATUS_GAME_LOADED || r->status == RA_STATUS_ACTIVE)
			return 0;
		if (r->status >= RA_STATUS_ERROR_WIFI)
			return -2;
	}
	return -1;
}

/* ------------------------------------------------------------------------
 * Phase 2a — synchronous pre-boot handshake.
 *
 * Called from main() right before the kernel tells the PPC loader it is
 * ready (BootStatus(0xdeadbeef)). While we block here, the loader sits in
 * its "Loading patched kernel..." wait loop and the GameCube game has NOT
 * started — exactly the window we want for game identification + achievement
 * fetch.
 *
 * For now this only validates the gating: a few IDENTIFY round-trips to
 * confirm the EXI link works at this pre-boot point, then a deliberate
 * multi-second delay simulating the ESP's RA-server fetch. On hardware we
 * expect the game boot to be visibly delayed by this block, with the
 * PRE-BOOT debug lines appearing on the ESP's COM4 BEFORE the game runs.
 *
 * Phase 2b will replace the simulated delay with: read disc header + DOL via
 * ReadRealDisc/ISORead, compute the RetroAchievements GameCube MD5, send
 * RA_CMD_LOAD_GAME (has_hash=1), and poll RA_CMD_POLL until GAME_LOADED.
 * ------------------------------------------------------------------------ */
void RA_PreBootHandshake(void)
{
	u8   hdr[32];
	char msg[80];
	int  i, o;

	led_setup();
	led_blink(3, 80);   /* entering pre-boot handshake */

	/* ISO/file games: the lazy ISOInit() in DIReadThread hasn't run yet at
	 * this pre-boot point, so init it ourselves. Disc games are already set
	 * up by RealDI_Init() during kernel init. */
	if (!RealDiscCMD && !FSTMode)
		ISOInit();

	/* Phase 2b-1: prove we can read the game image BEFORE boot. Read the
	 * disc header (32 bytes at offset 0), log the 6-char Game ID, the first
	 * 8 header bytes, and the GameCube magic word at 0x1C (must be C2339F3D
	 * for a valid disc — it's the precondition rc_hash_gamecube checks). */
	if (ra_disc_read(hdr, 0, 32) == 0) {
		o = 0;
		const char *p = "disc id=";
		while (*p) msg[o++] = *p++;
		for (i = 0; i < 6; i++) {
			u8 b = hdr[i];
			msg[o++] = (b >= 32 && b < 127) ? (char)b : '?';
		}
		msg[o] = 0;
		ra_debug(msg);

		o = 0; p = "magic@1C=";
		while (*p) msg[o++] = *p++;
		for (i = 0x1C; i < 0x20; i++) { msg[o++] = ra_hexd(hdr[i] >> 4); msg[o++] = ra_hexd(hdr[i] & 0xF); }
		msg[o] = 0;
		ra_debug(msg);
	} else {
		ra_debug("GC-RA disc read FAILED (pre-boot)");
	}

	/* Phase 2b-2: compute the RetroAchievements GameCube hash and log it.
	 * Reading the header + DOL segments from disc takes a couple of seconds,
	 * which also serves as the gating delay. */
	{
		char hex[33];
		char gid[7];
		int  hr;

		for (i = 0; i < 6; i++) gid[i] = (char)hdr[i];
		gid[6] = 0;

		hr = ra_compute_gc_hash(hex);
		if (hr == 0) {
			const char *p;
			int lr;

			o = 0; p = "ra hash=";
			while (*p) msg[o++] = *p++;
			for (i = 0; i < 32; i++) msg[o++] = hex[i];
			msg[o] = 0;
			ra_debug(msg);

			/* LOAD_GAME + poll until the ESP fetched the achievement set. */
			ra_debug("GC-RA LOAD_GAME -> polling for GAME_LOADED");
			lr = ra_load_game(gid, hex);
			/* The ESP is busy right at its game-loaded state transition
			 * (rc_client callback, its own Serial prints), so a debug sent
			 * immediately gets dropped. Let it settle before reporting. */
			mdelay(500);
			if (lr == 0)        ra_debug("GC-RA GAME_LOADED ok <<<");
			else if (lr == -2)  ra_debug("GC-RA LOAD_GAME error (ESP)");
			else                ra_debug("GC-RA LOAD_GAME timeout");
			mdelay(300);
		} else {
			const char *p;
			o = 0; p = "ra hash FAILED rc=-";
			while (*p) msg[o++] = *p++;
			msg[o++] = (char)('0' + (-hr));
			msg[o] = 0;
			ra_debug(msg);
		}
	}

	ra_debug("GC-RA PRE-BOOT done -> releasing game");
}

void RA_Init(void)
{
	/* Stack carved out of the kernel stack region by kernel.ld. */
	extern char __ra_stack_addr, __ra_stack_size;

	/* Priority 0x30 — BELOW the kernel main loop (0x50) and DI (0x78). The
	 * main loop services the running game's EXI/SI emulation; if RA ran at
	 * 0x50 (same as main), a long EXI burst (4 KB chunk read, 547-entry
	 * snapshot) with no yield blocks main and freezes the game. At 0x30 the
	 * main loop preempts RA's bursts, so the game stays serviced; RA runs in
	 * the main loop's idle gaps (it only needs ~30 Hz). */
	ra_thread_id = do_thread_create(RA_PollThread,
	                                (u32 *)&__ra_stack_addr,
	                                (u32)(&__ra_stack_size),
	                                RA_THREAD_PRIO);
	if ((s32)ra_thread_id >= 0)
		thread_continue(ra_thread_id);
}
