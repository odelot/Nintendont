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
 * NOTE: the Wii ra-module used 0x2FF8, but on GameCube that lands ABOVE
 * POffset (the patch area grows DOWN from PATCH_OFFSET_START = 0x2FF4), i.e.
 * in the patch-system's reserved 12-byte slot at 0x2FF4..0x3000 — NOT zeroed
 * and not ours. 0x1B00 sits safely inside the zeroed cheats region instead.
 * (RA overrides user GCT cheats when active, so GCT collision is moot.)
 * ------------------------------------------------------------------------ */
#define RA_USE_VBI            1
#define RA_VBI_COUNTER_PHYS   0x1B00u

/* ------------------------------------------------------------------------
 * Achievement celebration extras (feature toggles, default ON).
 *
 * RA_USE_OVERLAY — draw a small flash block into the game's framebuffer on an
 *   unlock. v2 (2026-06-16): the kernel only raises a flag at RA_OVERLAY_FLAG_PHYS;
 *   the BLIT happens on the PPC in PADReadGC — it reads VI_TFBL (0xCC00201C),
 *   decodes the XFB address, and writes a small white block UNCACHED straight
 *   into the displayed framebuffer, with interrupts on. Drawing on the PPC (not
 *   the Starlet) avoids the cross-core race + 50 KB cache flush that froze the
 *   v1 Starlet-blit version.
 *
 * RA_USE_RUMBLE — buzz the controller(s) on an unlock (kernel raises a flag,
 *   PADReadGC forces MotorCommand). DISABLED for now (froze + isolating overlay):
 *   re-enabling needs the PADReadGC MotorCommand-force re-added (it was removed).
 *
 * RA shared slots live in the codehandler's zeroed cheats area (0x1B00 VBI,
 * 0x1B08 overlay flag) — reserved, never touched by the game. */
#define RA_USE_OVERLAY        1
#define RA_OVERLAY_FLAG_PHYS  0x1B08u   /* Starlet writes; PADReadGC reads (0xC0001B08) */
#define RA_USE_RUMBLE         0
#define RA_RUMBLE_FLAG_PHYS   0x1B10u   /* Starlet writes; PADReadGC reads (0xC0001B10) */

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
