/*
 * rappc_shared.h — memory contract between the Nintendont kernel (Starlet)
 * and the RA PPC agent (rappc.bin, runs on the Broadway).
 *
 * WHY (2026-11-15, Majora's Mask in Zelda Collector's Edition): the Starlet is
 * shared with Nintendont's own emulation loop (~6 ms/frame) and the IOS (~5 ms);
 * reading ~2245 watched bytes + walking ~1600 pointer chains on the Starlet cost
 * ~9.5 ms, about 4 ms of it just keeping MEM1 coherent with the PPC's write-back
 * cache. The PPC sees its own cache, so it gathers everything at the vertical
 * retrace (inside the game's __VIRetraceHandler, right after retraceCount++)
 * into a slot here, and the Starlet only copies the slot into the SNAPSHOT.
 * The wire format to the ESP is unchanged.
 *
 * Area: MEM2 0x13170000-0x131BFFFF (PPC EA 0x93170000), free in Nintendont's
 * map (mem_map.txt: between the OSReport buffer 0x93160000 and the PSO PRS dol
 * 0x931C0000). The image is linked there; code first, then .bss with the shared
 * blocks. The Starlet reads every address from rappc_head_t (first bytes).
 *
 * Coherency rules:
 *  - CTL: written ONLY by the Starlet (cached + flush); the PPC reads it
 *    UNCACHED (EA + 0x40000000).
 *  - PST: written ONLY by the PPC, UNCACHED; the Starlet invalidates + reads.
 *  - Tables (flat addresses, chain nodes): Starlet writes + flushes, then bumps
 *    the gen in CTL; on a gen change the PPC dcbi's the range before reading.
 *  - Slots: written ONLY by the PPC (cached + dcbst before publishing).
 *  - Mutual exclusion for table/CTL updates: CTL.writing (Starlet) vs PST.busy
 *    (PPC), each side sets its flag then checks the other's (both uncached /
 *    flushed, with barriers): the Starlet waits while busy; the PPC skips the
 *    retrace when it sees writing.
 *
 * Both CPUs are big-endian; the structs are plain u32 so the layout is the same.
 */
#ifndef __RAPPC_SHARED_H__
#define __RAPPC_SHARED_H__

#define RAPPC_MAGIC        0x52415043u   /* 'RAPC' */
#define RAPPC_VERSION      1u
#define RAPPC_BASE_EA      0x93170000u
#define RAPPC_BASE_PHYS    0x13170000u
#define RAPPC_AREA_SIZE    0x00050000u
#define RAPPC_UNCACHED     0x40000000u   /* EA offset: 0x9xxxxxxx -> 0xDxxxxxxx */

#define RAPPC_MAX_FLAT     6144u         /* = RA_MAX_WATCH_ADDRS */
#define RAPPC_MAX_NODES    2048u         /* = RA_GC_CHAIN_NODES */
#define RAPPC_NSLOT        3u
#define RAPPC_NO_SLOT      0xFFu

/* first bytes of the image (section .rahead) */
typedef struct {
	unsigned int magic;
	unsigned int version;
	unsigned int entry;       /* EA of ra_ppc_entry */
	unsigned int ctl;         /* EA of the CTL block */
	unsigned int pst;         /* EA of the PST block */
	unsigned int flat_tab;    /* EA of u32[RAPPC_MAX_FLAT] */
	unsigned int node_tab;    /* EA of node[RAPPC_MAX_NODES] (8 bytes each) */
	unsigned int slot[RAPPC_NSLOT];
	unsigned int slot_size;
	unsigned int image_end;   /* EA just past .bss */
} rappc_head_t;

/* CTL — Starlet -> PPC (64 bytes, one 32-byte-aligned block of two lines) */
typedef struct {
	unsigned int magic;       /* RAPPC_MAGIC once initialised */
	unsigned int enable;      /* 0 = only publish the retrace count */
	unsigned int writing;     /* Starlet is changing tables/CTL: skip this retrace */
	unsigned int flat_gen;
	unsigned int flat_count;
	unsigned int chain_gen;
	unsigned int node_count;
	unsigned int committed;   /* slot the ESP got last = history for the next walk */
	unsigned int rt_count_ea; /* EA of the game's retraceCount (u32) */
	unsigned int pad[7];
} rappc_ctl_t;

/* PST — PPC -> Starlet (32 bytes) */
typedef struct {
	unsigned int busy;        /* PPC is walking */
	unsigned int latest;      /* newest complete slot, RAPPC_NO_SLOT = none */
	unsigned int seq;         /* retrace count of that slot */
	unsigned int runs;        /* walks done */
	unsigned int t_last;      /* timebase ticks of the last walk */
	unsigned int t_max;
	unsigned int t_sum;
	unsigned int skipped;     /* retraces without a walk (disabled / writing / bad CTL) */
} rappc_pst_t;

/* slot header (32 bytes) */
typedef struct {
	unsigned int seq;         /* retrace count sampled */
	unsigned int flat_gen;
	unsigned int chain_gen;
	unsigned int flat_count;
	unsigned int node_count;
	unsigned int hist;        /* slot used as walk history (RAPPC_NO_SLOT = none) */
	unsigned int have_prev;
	unsigned int done;        /* = seq once the slot is complete, 0 while written */
} rappc_slot_hdr_t;

/* slot layout (byte offsets, every section 32-byte aligned) */
#define RAPPC_SL_VALS      32u
#define RAPPC_SL_NVAL      (RAPPC_SL_VALS   + RAPPC_MAX_FLAT)
#define RAPPC_SL_NPREV     (RAPPC_SL_NVAL   + 4u * RAPPC_MAX_NODES)
#define RAPPC_SL_NPRIOR    (RAPPC_SL_NPREV  + 4u * RAPPC_MAX_NODES)
#define RAPPC_SL_NVALID    (RAPPC_SL_NPRIOR + 4u * RAPPC_MAX_NODES)
#define RAPPC_SL_NVP       (RAPPC_SL_NVALID + RAPPC_MAX_NODES / 8u)
#define RAPPC_SLOT_SIZE    (RAPPC_SL_NVP    + RAPPC_MAX_NODES / 8u)

#endif
