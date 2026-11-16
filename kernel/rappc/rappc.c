/*
 * rappc.c — RA PPC agent (Broadway side). See rappc_shared.h for the contract.
 *
 * Called once per vertical retrace from the game's __VIRetraceHandler (the
 * Starlet-installed stub right after retraceCount++), interrupts disabled, on a
 * private stack (ra_ppc_entry.S). Gathers the flat watchlist bytes and walks
 * the Phase C/D chain table — a line-by-line mirror of the Starlet's
 * ra_chain_walk (kernel/ra_module.c) — into a slot, then publishes the
 * retrace count at 0xC0001020 (what the old stub did).
 *
 * Built freestanding: no libc, no small-data (r2/r13 belong to the game), no
 * floating point.
 */
#include "rappc_shared.h"
#include "../gc_ra_protocol.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

typedef struct __attribute__((packed)) {
	u32 operand;
	u16 parent;
	u8  op;
	u8  psize;
} ra_node_t;

#define PARENT_NONE  0xFFFFu
#define RT_SLOT_UNC  0xC0001020u   /* RA_RETRACE_SLOT_PHYS, uncached EA */

/* shared blocks, ONE object so their order is fixed and the first-run dcbi
 * covers exactly them (zeroed by the Starlet before the first enable) */
typedef struct {
	rappc_ctl_t ctl;                               /* 64 */
	rappc_pst_t pst;                               /* 32 */
	u32         flat_tab[RAPPC_MAX_FLAT];
	ra_node_t   node_tab[RAPPC_MAX_NODES];
	u8          slots[RAPPC_NSLOT][RAPPC_SLOT_SIZE];
} ra_area_t;
ra_area_t ra_ppc_area __attribute__((aligned(32)));
u8        ra_ppc_stack[4096] __attribute__((aligned(32)));
#define ra_ppc_ctl       (ra_ppc_area.ctl)
#define ra_ppc_pst       (ra_ppc_area.pst)
#define ra_ppc_flat_tab  (ra_ppc_area.flat_tab)
#define ra_ppc_node_tab  (ra_ppc_area.node_tab)
#define ra_ppc_slots     (ra_ppc_area.slots)

/* PPC-private state — in .data (outside the dcbi-ed area) */
#define PRIV __attribute__((section(".data")))
#define PRIVV PRIV
static u32 my_flat_gen  PRIV = 0xFFFFFFFFu;
static u32 my_flat_n    PRIV = 0;
static u32 my_chain_gen PRIV = 0xFFFFFFFFu;
static u32 my_node_n    PRIV = 0;
static u32 first_run    PRIV = 1;

extern void ra_ppc_entry(void);
extern u8   ra_ppc_image_end[];

const rappc_head_t ra_ppc_head __attribute__((section(".rahead"), used)) = {
	RAPPC_MAGIC, RAPPC_VERSION,
	(u32)ra_ppc_entry,
	(u32)&ra_ppc_ctl, (u32)&ra_ppc_pst,
	(u32)ra_ppc_flat_tab, (u32)ra_ppc_node_tab,
	{ (u32)ra_ppc_slots[0], (u32)ra_ppc_slots[1], (u32)ra_ppc_slots[2] },
	RAPPC_SLOT_SIZE,
	(u32)ra_ppc_image_end,
};

static inline u32 mftb(void)
{
	u32 t;
	__asm__ volatile ("mftb %0" : "=r"(t));
	return t;
}

static inline void sync(void)
{
	__asm__ volatile ("sync" ::: "memory");
}

static void dcbi_range(const void *p, u32 n)
{
	u32 a = (u32)p & ~31u, e = (u32)p + n;
	for (; a < e; a += 32)
		__asm__ volatile ("dcbi 0,%0" :: "r"(a) : "memory");
	sync();
}

static void dcbst_range(const void *p, u32 n)
{
	u32 a = (u32)p & ~31u, e = (u32)p + n;
	for (; a < e; a += 32)
		__asm__ volatile ("dcbst 0,%0" :: "r"(a) : "memory");
	sync();
}

/* One byte of GC MEM1 — the Starlet's ra_read_mem1 semantics (address masked
 * to 32 MB, outside 24 MB reads 0) — WITHOUT allocating a cache line.
 *
 * A cached load here would pull the line into the PPC cache from inside the VI
 * interrupt. If the game had just invalidated that range for a disc read (the
 * SDK invalidates BEFORE the transfer), the line we fetched holds the OLD
 * bytes; after the transfer the game reads our stale line instead of the new
 * data (Melee froze loading the character select, 2026-11-15). So: dcbst the
 * line (writes back a dirty one — the game's latest bytes — and never
 * allocates), sync, then read through the cache-inhibited view. The game's
 * cache contents are untouched. last_line skips the dcbst for consecutive
 * reads of one line (the game is not running while we walk). */
static u32 last_line PRIVV = 0xFFFFFFFFu;
static u32 last_word PRIVV = 0xFFFFFFFFu;   /* one uncached load per word: a 16/32-bit */
static u32 last_w    PRIVV = 0;             /* memref is 2-4 consecutive watch entries */
static inline u8 rd8(u32 addr)
{
	u32 off = addr & 0x01FFFFFFu;
	if (off >= 0x01800000u)
		return 0;
	if ((off & ~3u) != last_word) {
		if ((off & ~31u) != last_line) {
			last_line = off & ~31u;
			__asm__ volatile ("dcbst 0,%0" :: "r"(0x80000000u | last_line) : "memory");
			__asm__ volatile ("sync" ::: "memory");
		}
		last_word = off & ~3u;
		last_w = *(volatile u32 *)(0xC0000000u | last_word);
	}
	return (u8)(last_w >> ((3u - (off & 3u)) * 8u));
}

/* ---- mirror of kernel/ra_module.c (ra_chain_xform/combine/addr_ok) ---- */
static u32 xform(u32 v, u8 psize)
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

static u32 combine(u32 pv, u32 o, u8 op)
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

static int addr_ok(u32 addr, u8 w)
{
	u32 end = addr + (u32)w - 1u;
	if (end < addr) return 0;
	if (end <= 0x017FFFFFu) return 1;
	return 0;
}

/* The walk: identical to ra_chain_walk, with "the previous walk" = slot hs
 * (the one the ESP got last) instead of in-place history arrays. */
static void walk(u8 *s, const u8 *hs, u32 nc)
{
	u32 *nv  = (u32 *)(s + RAPPC_SL_NVAL);
	u32 *np  = (u32 *)(s + RAPPC_SL_NPREV);
	u32 *npr = (u32 *)(s + RAPPC_SL_NPRIOR);
	u8  *vb  = s + RAPPC_SL_NVALID;
	u8  *vbp = s + RAPPC_SL_NVP;
	const u32 *hv  = hs ? (const u32 *)(hs + RAPPC_SL_NVAL)   : 0;
	const u32 *hpr = hs ? (const u32 *)(hs + RAPPC_SL_NPRIOR) : 0;
	const u8  *hvb = hs ? hs + RAPPC_SL_NVALID : 0;
	u32 have_prev = hs ? 1u : 0u;
	u32 i, nb = (nc + 7u) / 8u;

	/* validity of walk N-1 (ships with the dp section) */
	for (i = 0; i < nb; i++)
		vbp[i] = hvb ? hvb[i] : 0;

	for (i = 0; i < nc; i++) {
		const ra_node_t *n = &ra_ppc_node_tab[i];
		u8  op    = n->op & 0x1F;
		u8  dpk   = (u8)(n->psize >> RA_CN_PSZ_DP_SHIFT);
		u8  valid = 1;
		u32 pv    = 0;
		u32 v     = 0;
		u32 old   = hv  ? hv[i]  : 0;
		u32 oprio = hpr ? hpr[i] : 0;

		if (n->parent != PARENT_NONE) {
			u32 p = n->parent;
			if (p >= i || !(vb[p >> 3] & (1u << (p & 7))))
				valid = 0;
			else if (dpk != RA_CN_DP_CUR && !have_prev)
				valid = 0;
			else {
				u32 praw = (dpk == RA_CN_DP_DELTA) ? np[p]
				         : (dpk == RA_CN_DP_PRIOR) ? npr[p]
				         :                           nv[p];
				pv = xform(praw, (u8)(n->psize & RA_CN_PSZ_MEM_MASK));
			}
		}

		if (op == RA_CN_OP_NONE) {
			v = n->operand;
		}
		else if (op == RA_CN_OP_DEREF) {
			u32 addr = (n->parent == PARENT_NONE) ? n->operand : (pv + n->operand);
			u8 w = (u8)(((n->op >> 5) & 3) + 1);
			if (valid && addr_ok(addr, w)) {
				u8 k;
				for (k = 0; k < w; k++)
					v |= (u32)rd8(addr + k) << (8u * k);
			} else {
				valid = 0;
				v = 0;
			}
		}
		else {
			if (valid)
				v = combine(pv, n->operand, op);
		}

		np[i]  = old;
		npr[i] = (v != old) ? old : oprio;
		nv[i]  = v;
		if (valid)
			vb[i >> 3] |=  (u8)(1u << (i & 7));
		else
			vb[i >> 3] &= (u8)~(1u << (i & 7));
	}
}

void ra_ppc_main(void)
{
	volatile rappc_ctl_t *c = (volatile rappc_ctl_t *)((u32)&ra_ppc_ctl + RAPPC_UNCACHED);
	volatile rappc_pst_t *p = (volatile rappc_pst_t *)((u32)&ra_ppc_pst + RAPPC_UNCACHED);
	u32 t0 = mftb();
	u32 rt = c->rt_count_ea, count = 0;
	u32 fc, nc, cm, lt, w, i;
	u8 *s;
	const u8 *hs = 0;
	rappc_slot_hdr_t *h;

	/* First run: discard any line of our shared area this cache still holds
	 * from before the game (the loader ran on this CPU): a stale dirty line
	 * evicted later would overwrite what the Starlet writes. The private
	 * stack (in use right now) lies past ra_ppc_slots and is not touched. */
	if (first_run) {
		first_run = 0;
		dcbi_range(&ra_ppc_area, sizeof(ra_ppc_area));
		/* the rest of the RA area past our stack (the Starlet's snapshot
		 * buffer lives there): no stale line of ours may land on it either */
		dcbi_range(ra_ppc_image_end, (RAPPC_BASE_EA + RAPPC_AREA_SIZE) - (u32)ra_ppc_image_end);
	}

	if ((rt >> 25) == (0x80000000u >> 25) && !(rt & 3u))   /* 0x80000000-0x81FFFFFF */
		count = *(volatile u32 *)rt;

	if (c->magic != RAPPC_MAGIC || !c->enable)
		goto skip;

	p->busy = 1;
	sync();
	if (c->writing) {
		p->busy = 0;
		sync();
		goto skip;
	}

	fc = c->flat_count;
	nc = c->node_count;
	cm = c->committed;
	if (fc > RAPPC_MAX_FLAT || nc > RAPPC_MAX_NODES) {
		p->busy = 0;
		sync();
		goto skip;
	}
	/* tables changed: drop our cached copy of them */
	if (c->flat_gen != my_flat_gen || fc != my_flat_n) {
		dcbi_range(ra_ppc_flat_tab, (fc ? fc : 1u) * 4u);
		my_flat_gen = c->flat_gen;
		my_flat_n   = fc;
	}
	if (c->chain_gen != my_chain_gen || nc != my_node_n) {
		dcbi_range(ra_ppc_node_tab, (nc ? nc : 1u) * 8u);
		my_chain_gen = c->chain_gen;
		my_node_n    = nc;
	}

	/* history = the slot the ESP got last, if it belongs to this chain table */
	if (cm < RAPPC_NSLOT) {
		const rappc_slot_hdr_t *hh = (const rappc_slot_hdr_t *)ra_ppc_slots[cm];
		if (hh->chain_gen == my_chain_gen && hh->node_count == nc && hh->done != 0)
			hs = ra_ppc_slots[cm];
		else
			cm = RAPPC_NO_SLOT;
	} else {
		cm = RAPPC_NO_SLOT;
	}
	/* write into a slot that is neither the history nor the newest one */
	lt = p->latest;
	for (w = 0; w < RAPPC_NSLOT; w++)
		if (w != cm && w != lt)
			break;

	s = ra_ppc_slots[w];
	h = (rappc_slot_hdr_t *)s;
	h->done = 0;                     /* torn-read marker, in memory before any data */
	dcbst_range(h, 32);

	last_line = 0xFFFFFFFFu;          /* the game ran since the last walk */
	last_word = 0xFFFFFFFFu;
	for (i = 0; i < fc; i++)
		s[RAPPC_SL_VALS + i] = rd8(ra_ppc_flat_tab[i]);
	if (nc)
		walk(s, hs, nc);

	h->seq        = count;
	h->flat_gen   = my_flat_gen;
	h->chain_gen  = my_chain_gen;
	h->flat_count = fc;
	h->node_count = nc;
	h->hist       = cm;
	h->have_prev  = hs ? 1u : 0u;
	h->done       = count ? count : 1u;
	dcbst_range(s + RAPPC_SL_VALS, fc ? fc : 1u);
	if (nc) {
		dcbst_range(s + RAPPC_SL_NVAL,   nc * 4u);
		dcbst_range(s + RAPPC_SL_NPREV,  nc * 4u);
		dcbst_range(s + RAPPC_SL_NPRIOR, nc * 4u);
		dcbst_range(s + RAPPC_SL_NVALID, 2u * (RAPPC_MAX_NODES / 8u));
	}
	dcbst_range(h, 32);

	p->latest = w;
	p->seq    = count;
	p->runs   = p->runs + 1;
	{
		u32 dt = mftb() - t0;
		p->t_last = dt;
		p->t_sum  = p->t_sum + dt;
		if (dt > p->t_max) p->t_max = dt;
	}
	sync();
	p->busy = 0;
	sync();
	goto publish;

skip:
	p->skipped = p->skipped + 1;
publish:
	/* what the old stub did: the retrace count, uncached, for the Starlet's
	 * clock — written LAST, so a new count means its slot is complete */
	*(volatile u32 *)RT_SLOT_UNC = count;
	sync();
}
