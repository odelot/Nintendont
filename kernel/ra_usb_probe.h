/*
 * ra_usb_probe.h — USB transport SPIKE, kernel side (see ra_usb_probe.c and
 * common/include/RAUsbProbe.h). Disposable; gated by RA_USB_PROBE.
 */

#ifndef _RA_USB_PROBE_KERNEL_H_
#define _RA_USB_PROBE_KERNEL_H_

/* Fallback probe thread priority (the RA thread's tier, RA_THREAD_PRIO 0x78).
 * RA_USBP_Start first tries 0x7F / 0x7C, then steps down per pass (see
 * usbp_step_priority). If the game stutters, suspect priority before the USB
 * (the analogue of the Wii's ios_thread_priority lesson). */
#define RA_USBP_PRIO            0x78

/* Iterations per matrix row, and how long the idle result stays on the
 * loader screen before the game is released. */
#define RA_USBP_ITERS           200
#define RA_USBP_IDLE_HOLD_MS    10000

/* In-game schedule: wait for the game to boot, then loop forever over
 * { full matrix, SOAK_SECS of 60 Hz regime traffic reported every WINDOW }. */
#define RA_USBP_GAME_DELAY_MS   15000
#define RA_USBP_SOAK_SECS       60
#define RA_USBP_WINDOW_SECS     5

/* Synchronous idle pass. Call from main() before the kernel releases the
 * game (the loader is still drawing its status screen). No-op when the
 * loader did not hand over a device. */
void RA_USBP_Idle(void);

/* Spawn the in-game probe thread. Call from main() where RA_Init() would
 * go. Skipped when the RetroAchievements setting is On (shared stack). */
void RA_USBP_Start(void);

#endif /* _RA_USB_PROBE_KERNEL_H_ */
