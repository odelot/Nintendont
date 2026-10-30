# Nintendont — RetroAchievements Fork

> **This is a fork of [Nintendont](https://github.com/FIX94/Nintendont)** that adds
> [RetroAchievements](https://retroachievements.org) support for **GameCube games** played on a
> real Wii, via an external ESP32-S3 adapter plugged into a Wii **USB port**.
>
> It is one of four cooperating projects; see the
> [**wii-ra-adapter**](https://github.com/odelot/wii-ra-adapter) repository for the system overview
> and for the **pre-built binaries of all four projects** (releases are published there).

> **USB is the default link.** The adapter now plugs into a Wii USB port with a plain cable — no
> memory card connector, no soldering. This build uses the USB adapter when the loader finds one
> and otherwise falls back to the original memory card Slot B link (details collapsed below).

---

## What this fork adds

The RA integration lives almost entirely in the **Nintendont kernel** (the ARM code running on
Starlet next to the game), with a small loader hand-off and an assembly extension in the cheat
codehandler:

| File | Purpose |
|---|---|
| `kernel/ra_module.c` / `ra_module.h` | The whole RA engine: pre-boot handshake, GameCube RA hash, link selection, watchlist state machine, pointer-chain walker, per-frame SNAPSHOT loop, video-retrace clock, unlock celebration. |
| `kernel/ra_usb_link.c` / `ra_usb_link.h` | **USB link** (default): bulk exchanges with the adapter on the kernel's shared `/dev/usb/ven` handle (`USB_BlkMsgAsync`), with a timeout and the wire rules below. Twin of the d2x `usb_link.c`. |
| `loader/source/ra_usb_probe.c` + `common/include/RAUsbProbe.h` | **Loader hand-off**: with RA enabled, the PPC loader finds the adapter (VID:PID `303A:4052`) in the USB device list before closing its devices, writes the device id and endpoints to a hand-off block at `0x132C1100` for the kernel, then releases `/dev/usb/ven` (`RA_USBP_Release()`). |
| `kernel/Patch.c` | Calls `RA_RetraceHook()` right after the codehandler is copied, to hook the game's video-retrace counter (see *Frame sync*). |
| `kernel/gc_ra_protocol.h` | Shared binary protocol (kept in sync with the copy in wii-ra-adapter). |
| `kernel/main.c` | Two calls wired in: `RA_PreBootHandshake()` before the kernel signals the PPC loader it is ready, and `RA_Init()` before the kernel main loop. |
| `codehandler/codehandleronly.s` | Per-frame hook: draws a 32×32 gold trophy badge when the flag at `0x1008` is set, and keeps a framebuffer-flip counter at `0x1004` (now only a fallback clock). |
| `RA_PORT_PLAN.md` | The original feasibility analysis and porting plan from the Wii (d2x) implementation. |

Unlike the Wii version (an IOS module in the d2x cIOS), there is no separate loader project and
no IOS reload: Nintendont is loader, kernel and memory server in one, so everything happens here.

## How it works

### Pre-boot handshake (synchronous)

`RA_PreBootHandshake()` runs **before** the kernel releases the PPC loader
(`BootStatus(0xdeadbeef)`), i.e. while the game has not started yet — the ideal window for game
identification:

1. Picks the link: **USB** if the loader handed off an adapter, else memory card Slot B
   (`ra_link_select()`, logged on the adapter as `GC-RA link: USB` / `EXI (Slot B)`).
2. Reads the disc header through whichever backend the user is on — **physical disc**
   (`ReadRealDisc`), **ISO/GCM image** or **extracted FST** — and validates the GameCube magic
   word.
3. Computes the **RetroAchievements GameCube hash** in-kernel: a byte-for-byte port of rcheevos'
   `rc_hash_gamecube()` (header + apploader region capped at 1 MB, then every DOL segment),
   streamed through MD5 in 32 KB chunks. All disc reads are 32-byte aligned (a DI hardware
   requirement) with local slicing. Matching rcheevos exactly is what lets the RA server resolve
   any game its database knows — no hardcoded game tables.
4. Sends `LOAD_GAME` (game ID + hash) to the ESP32 and polls until it reports **GAME_LOADED**
   (the ESP32 is fetching the achievement set over Wi-Fi — up to ~90 s on a cold load), then
   releases the boot. If the adapter is absent or errors, the game boots normally without RA.

### Runtime: the kernel-side memory server

`RA_Init()` spawns a background kernel thread (priority 0x78, same tier as the DI/HID threads —
created high because IOS threads can only lower their own priority) that is a port of the d2x
`ra-module` with the same protocol and features:

- **USB link** — the kernel opens (or reuses, when USB controllers/storage already did)
  `/dev/usb/ven` and talks to the device id from the loader hand-off. Every request is one bulk
  OUT message and gets exactly one bulk IN reply holding the bytes an EXI read would have
  returned (the reply read is posted first); no message is a multiple of 64 bytes (a `0x00` is
  appended, so a short packet ends it); replies arrive trimmed to a multiple of 4 and the ESP32
  pads them; a transfer may cross at most one 4 KB page (the Wii's OHCI does not split), so the
  snapshot buffer is 4096-aligned and capped at 8191 bytes. GameCube snapshots are small
  (~1–2 KB), so exchanges are synchronous — no pipelining needed. A timer token on the same
  message queue bounds each wait; a timed-out exchange is finished before the next one starts.
- **Watchlist + chain walker** — fetches the watched-address list in chunks, then the Phase C
  pointer-chain descriptor table; every frame it walks the chains in game RAM (with per-node
  delta/prior histories, validity bitmaps and verification values — see the d2x fork README for
  the full description), samples the flat watched bytes from MEM1, and ships everything in one
  SNAPSHOT message (up to 8 KB).
- **Convergence** — drains up to 12 `ADDR_QUERY`/mutation rounds per frame, applying seq-verified
  watchlist appends/removals, exactly like the Wii module.
- **Frame sync: the game's own video retrace** — emulators run one frame per video field
  (59.94 Hz NTSC / 50 Hz PAL), so that is the clock achievement sets assume. At patch time
  `RA_RetraceHook()` finds the Dolphin SDK's `retraceCount++` inside the game's
  `__VIRetraceHandler` (SDA base from `__init_registers`, an anchor on the VI register block, the
  `lwz`/`addi`/`stw` increment, confirmed by the `PreCB(retraceCount)` load that follows) and
  redirects that store through an 8-instruction stub at `0x1040` that also publishes the count
  **uncached** at `0x1020` (a cached store would sit in Broadway's write-back cache). The kernel
  accepts it only after measuring 45–65 Hz for one second; from then on each snapshot is taken
  right after a retrace and numbered by it. If no hook is found, the rate is wrong, or the count
  stalls for 3 s, it falls back to the framebuffer-flip counter (trusted only after 30 advances)
  and then to a 60 Hz `HW_TIMER` clock. RA forces the cheat codehandler to load even with no user
  cheats (user GCT cheats are overridden while RA is active).
- **Unlock celebration** — non-blocking, driven over the snapshot loop: the disc-slot LED and the
  on-screen trophy badge blink 3× together (the kernel raises the flag at `0x1008`; the
  codehandler draws the badge into both framebuffers so it survives double buffering). Rumble is
  present but disabled.

<details>
<summary><b>Memory card (EXI) link — legacy adapters</b></summary>

Used when the loader found no USB adapter. Nintendont emulates memory cards purely in software
(game EXI accesses are patched to a fake register block at `0x13026800`), so the *real* Hollywood
EXI controller (`0x0d806800`) is free. The kernel drives channel 1 (Slot B, 8 MHz) directly with
`read32`/`write32` — kernel mode, no SWI indirection — and ships each snapshot in one 8 KB
INT-handshake ("Phase B") transaction.

</details>

### What the GameCube port gets "for free" vs the Wii

The Wii implementation (d2x `ra-module`) runs as a **user-mode IOS module**, which cost it SWI
indirection for every hardware register, a `.rodata`-less loader, and delicate coexistence with
IOS. The Nintendont kernel runs in **SVC mode** with a normal linker script, an idle real EXI bus
and its own `/dev/usb/ven` handle — the biggest pain points gone. The full analysis, including the
cache-coherency risk of reading game MEM1 from the ARM side, is in
[`RA_PORT_PLAN.md`](RA_PORT_PLAN.md).

## Building

Same as upstream (see *Compiling* below; the `docker-build.sh` wrapper pins a working devkitPro
image). The USB link is on by default (`RA_USB_LINK 1` in `common/include/RAUsbProbe.h`).
Pre-built binaries are published in the
[wii-ra-adapter releases](https://github.com/odelot/wii-ra-adapter/releases).

---

# Original Nintendont README

### Nintendont
A Wii Homebrew Project to play GC Games on Wii and vWii on Wii U

### Features:
* Works on Wii and Wii U (in vWii mode)
* Full-speed loading from a USB device or an SD card.
* Loads 1:1 and shrunken .GCM/.ISO disc images.
* Loads games as extracted files (FST format)
* Loads CISO-format disc images. (uLoader CISO format)
* Memory card emulation
* Play audio via disc audio streaming
* Bluetooth controller support (Classic Controller (Pro), Wii U Pro Controller)
* HID controller support via USB
* Custom button layout when using HID controllers
* Cheat code support
* Changeable configuration of various settings
* Reset/Power off via button combo (R + Z + Start) (R + Z + B + D-Pad Down)
* Advanced video mode patching, force progressive and force 16:9 widescreen
* Auto boot from loader
* Disc switching
* Use the official Nintendo GameCube controller adapter
* BBA Emulation (see [BBA Emulation Readme](BBA_Readme.md))

### Features: (Wii only)
* Play retail discs
* Play backups from writable DVD media (Old Wii only)
* Use real memory cards
* GBA-Link cable
* WiiRd
* Allow use of the Nintendo GameCube Microphone

### What Nintendont will never support:
* Game Boy Player

### Quick Installation:
1. Get the [loader.dol](loader/loader.dol?raw=true), rename it to boot.dol and put it in /apps/Nintendont/ along with the files [meta.xml](nintendont/meta.xml?raw=true) and [icon.png](nintendont/icon.png?raw=true).
2. Copy your GameCube games to the /games/ directory. Subdirectories are optional for 1-disc games in ISO/GCM and CISO format.
   * For 2-disc games, you should create a subdirectory /games/MYGAME/ (where MYGAME can be anything), then name disc 1 as "game.iso" and disc 2 as "disc2.iso".
   * For extracted FST, the FST must be located in a subdirectory, e.g. /games/FSTgame/sys/boot.bin .
3. Connect your storage device to your Wii or Wii U and start The Homebrew Channel.
4. Select Nintendont.

### Compiling:
For compile Nintendont yourself, get the following versions of the toolchain compiling PPC tools:
* **devkitARM r53-1**
* **devkitPPC r35-2**
* **libOGC 1.8.23-1**

These versions can be downloaded here: https://www.mediafire.com/folder/j0juqb5vvd6z5/devkitPro_archives

On Windows, run the "Build.bat" batch script for build Nintendont.

On Unix, run the "Build.sh" script.

Please use these specific versions for compiling Nintendont, **because if you try to compile them on latest dkARM/dkPPC/libOGC, you'll get a lot of compiler warnings and your build will crash when attemping to return to Nintendont menu**, so be warned about that.

### Notes
* The Wii and Wii U SD card slot is known to be slow. If you're using an SD card and are having performance issues, consider either using a USB SD reader or a USB hard drive.
* USB flash drives are known to be problematic.
* Nintendont runs best with storage devices formatted with 32 KB clusters. (Use either FAT32 or exFAT.)
