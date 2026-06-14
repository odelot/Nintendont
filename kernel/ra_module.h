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
