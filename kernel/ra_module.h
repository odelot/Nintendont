/*
 * ra_module.h — RetroAchievements memory server inside the Nintendont kernel.
 *
 * Port of the Wii d2x-cios ra-module (Starlet ARM) to Nintendont's kernel.
 * Unlike the Wii version, this runs in kernel/SVC mode so it touches the
 * real Hollywood EXI controller (0xd806800) directly via read32/write32 —
 * no MLOAD SWI indirection, and .rodata loads normally.
 *
 * The ESP32 (wii-ra-adapter) lives in memory-card Slot B = EXI channel 1,
 * device 0, exactly as on the Wii. Nintendont emulates the GameCube memory
 * cards entirely in software (it patches the game's EXI access to a fake
 * register block at 0x13026800 and never drives the real controller), so
 * the physical EXI bus on channel 1 is ours to use.
 *
 * Phase 0: spawn a background thread that proves life (slot-LED blink),
 * reads the EXI bus-master state, and performs IDENTIFY round-trips against
 * the ESP — confirming the real EXI bus is free and drivable.
 */

#ifndef _RA_MODULE_H_
#define _RA_MODULE_H_

/* ------------------------------------------------------------------------
 * VBlank frame-sync (feature toggle, default ON).
 *
 * When set, RA forces Nintendont's codehandler (the cheat engine) to load +
 * hook the game's OSSleepThread/PADHook (≈ once per game frame). The
 * codehandler increments a u32 counter at MEM1 phys RA_VBI_COUNTER_PHYS via an
 * UNCACHED PPC write each frame; the kernel polls it and uses it as the
 * frame_counter shipped to the ESP — so rc_client_do_frame runs once per real
 * GAME frame, exactly like the Dolphin emulator (most faithful to how RA
 * achievements are authored).
 *
 * If the counter never advances (game lacks the hook pattern, codehandler not
 * installed), the kernel falls back to the HW_TIMER 60 Hz wall-clock automatically.
 *
 * Set to 0 to disable the codehandler force entirely and always use HW_TIMER.
 *
 * Counter address: 0x1B00 — inside the codehandler's cheats area, just above
 * the codehandler code (~0x1AB8) and its 8-byte codelist at cheats_start
 * (~0x1AB0). That whole area is memset to 0 + flushed when RA forces the
 * codehandler, and the game never touches Nintendont's reserved cheats space.
 *
 * Slots now live in the codehandler's cheatdata scratch (FIXED at the start of
 * the blob, 0x1004/0x1008/0x100C), so they don't move as the codehandler grows
 * (the trophy badge added a lot of code). Zeroed + unused without a GCT.
 * (RA overrides user GCT cheats when active, so GCT collision is moot.)
 * ------------------------------------------------------------------------ */
#define RA_USE_VBI            1
#define RA_VBI_COUNTER_PHYS   0x1004u

/* ------------------------------------------------------------------------
 * Achievement TROPHY OVERLAY (feature toggle, default ON). v3 (2026-06-22):
 * ported from the WORKING WiiFlow ra_trophy_hook.s. The BADGE is drawn on the
 * PPC, inside the codehandler (runs per game frame), into BOTH the current and
 * previous VI_TFBL so it survives double-buffering — the piece the earlier
 * Starlet/PADReadGC attempts were missing. The kernel only raises a flag at
 * RA_TROPHY_FLAG_PHYS during the celebration (in sync with the LED blink); the
 * codehandler draws a 32x32 gold trophy badge in the top-left whenever the flag
 * is set. RA_USE_OVERLAY gates whether the kernel raises the flag (the badge
 * asm is always present but dormant when the flag stays 0).
 *
 * RA_USE_RUMBLE — DISABLED (froze earlier; PADReadGC MotorCommand-force removed). */
#define RA_USE_OVERLAY        1
#define RA_TROPHY_FLAG_PHYS   0x1008u   /* Starlet writes; codehandler reads (0xC0001008) */
#define RA_USE_RUMBLE         0
#define RA_RUMBLE_FLAG_PHYS   0x1B10u   /* (rumble disabled) */

/* Spawn the RA background thread. Call once from main(), before the kernel
 * main loop, after EXIInit()/the GPIO setup. */
void RA_Init(void);

/* Phase 2a: synchronous pre-boot handshake. Call from main() BEFORE the
 * kernel signals the PPC loader that it is ready (BootStatus(0xdeadbeef)),
 * so the game launch waits until this returns. For now it only proves the
 * gating (IDENTIFY + a deliberate fetch-simulation delay); Phase 2b adds the
 * real GameCube hash + LOAD_GAME + GAME_LOADED poll here. */
void RA_PreBootHandshake(void);

#endif /* _RA_MODULE_H_ */
