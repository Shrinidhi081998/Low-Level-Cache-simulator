#include "cache.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Translate internal MESI enum to display text used in dumps/debug.
 * This is presentation-only and does not affect simulation behavior.
 */
static const char *mesi_to_str(mesi_state_t s) 
{
    switch (s) 
    {
        case MESI_S:
            return "S";
        case MESI_E:
            return "E";
        case MESI_M:
            return "M";
        case MESI_I:
        default:
            return "I";
    }
}

/*
 * Decode set index from 32-bit address:
 * [ tag | set | byte-offset ]
 * set bits are selected using BYTE_BITS and SET_MASK constants.
 */
static uint32_t addr_set(uint32_t addr) 
{
    return (addr >> BYTE_BITS) & SET_MASK;
}

static uint16_t addr_tag(uint32_t addr) 
{
    return (uint16_t)(addr >> TAG_SHIFT);
}

/*
 * Reconstruct block-aligned address from tag+set, used for L2 logs when
 * evicting or responding to snoop operations.
 */
static uint32_t line_addr(uint16_t tag, uint32_t set) 
{
    return ((uint32_t)tag << TAG_SHIFT) | (set << BYTE_BITS);
}

/*
 * Line storage is a flat array of size (SET_COUNT * ways).
 * These helpers map conceptual [set][way] indexing to flat offsets.
 */
static cache_line_t *cache_line_at(cache_t *cache, uint32_t set, uint32_t way) 
{
    return &cache->lines[(size_t)set * cache->ways + way];
}

static const cache_line_t *cache_line_at_const(const cache_t *cache, uint32_t set, uint32_t way)
{
    return &cache->lines[(size_t)set * cache->ways + way];
}

/*
 * Dual-output logger:
 * 1) Always writes to console (vprintf)
 * 2) Also writes to cfg->log_fp when configured
 * Uses va_list twice, so va_start/va_end is done once per sink.
 */
void cache_log_printf(const sim_cfg_t *cfg, const char *fmt, ...) 
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    if (cfg && cfg->log_fp) 
    {
        va_start(args, fmt);
        vfprintf(cfg->log_fp, fmt, args);
        va_end(args);
        fflush(cfg->log_fp);
    }
}

/*
 * LRU update when an existing line is touched (hit path):
 * - Capture old LRU rank of touched line.
 * - Any valid peer with rank > old rank is shifted down by one.
 * - Touched line becomes MRU rank 7.
 * This preserves relative order among unaffected lines.
 */
static void lru_touch_existing(cache_t *cache, uint32_t set, uint32_t touched_way)
{
    cache_line_t *touched = cache_line_at(cache, set, touched_way);
    uint32_t way;
    uint16_t old;

    old = touched->lru;

    for (way = 0; way < cache->ways; ++way) 
    {
        cache_line_t *line = cache_line_at(cache, set, way);
        if (!line->valid || way == touched_way)
        {
            continue;
        }
        if (line->lru > old)
        {
            --line->lru;
        }
    }
    touched->lru = 7;
}

/*
 * LRU update when installing a brand new line:
 * - Every other valid line ages by one level (down to minimum 0).
 * - Newly installed line is marked MRU rank 7.
 */
static void lru_touch_new(cache_t *cache, uint32_t set, uint32_t touched_way)
{
    uint32_t way;
    cache_line_t *touched = cache_line_at(cache, set, touched_way);

    for (way = 0; way < cache->ways; ++way) 
    {
        cache_line_t *line = cache_line_at(cache, set, way);
        if (!line->valid || way == touched_way) 
        {
            continue;
        }
        if (line->lru > 0) 
        {
            --line->lru;
        }
    }
    touched->lru = 7;
}

/*
 * Tag match lookup in one set.
 * A line is considered a hit only when:
 * - valid bit is set
 * - tags match
 * - MESI is not I
 */
static int find_hit_way(cache_t *cache, uint32_t set, uint16_t tag)
{
    uint32_t way;
    for (way = 0; way < cache->ways; ++way)
    {
        cache_line_t *line = cache_line_at(cache, set, way);
        if (line->valid && line->tag == tag && line->mesi != MESI_I)
        {
            return (int)way;
        }
    }
    return -1;
}

/*
 * Choose a replacement way in two passes:
 * 1) Prefer invalid ways (lowest LRU among invalids).
 * 2) If set is full, evict least-recently-used valid way (minimum LRU).
 */
static int pick_victim_way(cache_t *cache, uint32_t set)
{
    uint32_t way;
    int victim = -1;
    uint16_t min_lru = 0xFFFF;

    for (way = 0; way < cache->ways; ++way)
    {
        cache_line_t *line = cache_line_at(cache, set, way);
        if (!line->valid)
        {
            if (line->lru < min_lru)
            {
                min_lru = line->lru;
                victim = (int)way;
            }
        }
    }
    if (victim >= 0)
    {
        return victim;
    }

    min_lru = 0xFFFF;
    for (way = 0; way < cache->ways; ++way)
    {
        cache_line_t *line = cache_line_at(cache, set, way);
        if (line->lru < min_lru)
        {
            min_lru = line->lru;
            victim = (int)way;
        }
    }
    return victim;
}

/*
 * Emit L2 traffic lines only when mode >= 1.
 * Mode 0 keeps output minimal (summary + explicit dumps).
 */
static void maybe_log(sim_cfg_t cfg, const char *op, uint32_t addr)
{
    if (cfg.mode >= 1)
    {
        cache_log_printf(&cfg, "  [L2] %-22s 0x%08" PRIX32 "\n", op, addr);
    }
}

/*
 * Fill a cache line on allocation/replacement.
 * Resets write-tracking flags, sets tag/MESI, and marks as most-recently used.
 */
static void install_line(cache_t *cache, uint32_t set, uint32_t way, uint16_t tag, mesi_state_t mesi)
{
    cache_line_t *line = cache_line_at(cache, set, way);
    line->valid = true;
    line->dirty = false;
    line->first_write_done = false;
    line->tag = tag;
    line->mesi = mesi;
    line->lru = 7;
    lru_touch_new(cache, set, way);
}

/*
 * On eviction, write back only if line can contain newer data than L2:
 * dirty bit set OR MESI Modified.
 */
static void evict_if_needed(cache_t *cache, sim_cfg_t cfg, uint32_t set, uint32_t way)
{
    cache_line_t *victim = cache_line_at(cache, set, way);
    if (!victim->valid || victim->mesi == MESI_I)
    {
        return;
    }
    if (victim->dirty || victim->mesi == MESI_M)
    {
        maybe_log(cfg, "Write to L2", line_addr(victim->tag, set));
    }
}

/*
 * After a line is allocated, first write is logged as Write to L2 once.
 * `first_write_done` prevents duplicate first-write logs.
 */
static void write_through_if_first(cache_t *dc, sim_cfg_t cfg, uint32_t set, uint32_t way, uint32_t addr)
{
    cache_line_t *line = cache_line_at(dc, set, way);
    if (!line->first_write_done)
    {
        maybe_log(cfg, "Write to L2", addr);
        line->first_write_done = true;
    }
}

/*
 * Initialize cache metadata and allocate line storage.
 * Returns 1 on success, 0 on allocation failure.
 */
int cache_init(cache_t *cache, const char *name, uint32_t ways)
{
    cache->name = name;
    cache->ways = ways;
    cache->lines = (cache_line_t *)calloc((size_t)SET_COUNT * ways, sizeof(cache_line_t));
    if (!cache->lines)
    {
        return 0;
    }
    memset(&cache->stats, 0, sizeof(cache->stats));
    return 1;
}

/* Release allocated line storage. */
void cache_free(cache_t *cache)
{
    free(cache->lines);
    cache->lines = NULL;
}

/*
 * Reset cache to power-on-like empty state:
 * - all lines invalid (MESI=I, valid=false)
 * - LRU bits set to 111
 * - stats cleared
 */
void cache_reset(cache_t *cache)
{
    size_t total = (size_t)SET_COUNT * cache->ways;
    size_t i;

    memset(cache->lines, 0, total * sizeof(cache_line_t));
    for (i = 0; i < total; ++i)
    {
        cache->lines[i].lru = 7;
        cache->lines[i].mesi = MESI_I;
    }
    memset(&cache->stats, 0, sizeof(cache->stats));
}

/*
 * L1D read operation:
 * - hit: update stats, apply E->S rule, refresh LRU
 * - miss: read from L2, evict if needed, install as E
 * Returns 1 on hit, 0 on miss.
 */
int cache_data_read(cache_t *dc, sim_cfg_t cfg, uint32_t addr)
{
    uint32_t set = addr_set(addr);
    uint16_t tag = addr_tag(addr);
    int hit_way;

    dc->stats.reads++;
    hit_way = find_hit_way(dc, set, tag);
    if (hit_way >= 0)
    {
        cache_line_t *line = cache_line_at(dc, set, (uint32_t)hit_way);
        dc->stats.hits++;
        /* read hit demotes E to S. */
        if (line->mesi == MESI_E)
        {
            line->mesi = MESI_S;
        }
        lru_touch_existing(dc, set, (uint32_t)hit_way);
        return 1;
    }

    dc->stats.misses++;
    /* Read miss fetches line from L2. */
    maybe_log(cfg, "Read from L2", addr);

    {
        int victim = pick_victim_way(dc, set);
        evict_if_needed(dc, cfg, set, (uint32_t)victim);
        install_line(dc, set, (uint32_t)victim, tag, MESI_E);
    }
    return 0;
}

/*
 * L1I read operation (same lookup/replace policy as L1D read):
 * - hit: update stats, apply E->S, refresh LRU
 * - miss: fetch from L2 and install
 * Returns 1 on hit, 0 on miss.
 */
int cache_instr_read(cache_t *ic, sim_cfg_t cfg, uint32_t addr) 
{
    uint32_t set = addr_set(addr);
    uint16_t tag = addr_tag(addr);
    int hit_way;

    ic->stats.reads++;
    hit_way = find_hit_way(ic, set, tag);
    if (hit_way >= 0)
    {
        cache_line_t *line = cache_line_at(ic, set, (uint32_t)hit_way);
        ic->stats.hits++;
        /* Keep instruction-cache read semantics aligned with data read. */
        if (line->mesi == MESI_E)
        {
            line->mesi = MESI_S;
        }
        lru_touch_existing(ic, set, (uint32_t)hit_way);
        return 1;
    }

    ic->stats.misses++;
    /* Instruction read miss fetches from L2. */
    maybe_log(cfg, "Read from L2", addr);

    {
        int victim = pick_victim_way(ic, set);
        evict_if_needed(ic, cfg, set, (uint32_t)victim);
        install_line(ic, set, (uint32_t)victim, tag, MESI_E);
    }
    return 0;
}

/*
 * L1D write operation:
 * - hit: may request ownership if S, then transition to M + dirty
 * - miss: request ownership, install as M + dirty
 * Returns 1 on hit, 0 on miss.
 */
int cache_data_write(cache_t *dc, sim_cfg_t cfg, uint32_t addr)
{
    uint32_t set = addr_set(addr);
    uint16_t tag = addr_tag(addr);
    int hit_way;

    dc->stats.writes++;
    hit_way = find_hit_way(dc, set, tag);
    if (hit_way >= 0)
    {
        cache_line_t *line = cache_line_at(dc, set, (uint32_t)hit_way);
        dc->stats.hits++;
        /* Shared line needs ownership before write. */
        if (line->mesi == MESI_S)
        {
            maybe_log(cfg, "Read for Ownership from L2", addr);
        }
        /* Writes end in Modified + dirty. */
        line->mesi = MESI_M;
        line->dirty = true;
        write_through_if_first(dc, cfg, set, (uint32_t)hit_way, addr);
        lru_touch_existing(dc, set, (uint32_t)hit_way);
        return 1;
    }

    dc->stats.misses++;
    /* Write miss requests ownership and installs Modified line. */
    maybe_log(cfg, "Read for Ownership from L2", addr);

    {
        int victim = pick_victim_way(dc, set);
        cache_line_t *line;
        evict_if_needed(dc, cfg, set, (uint32_t)victim);
        install_line(dc, set, (uint32_t)victim, tag, MESI_M);
        line = cache_line_at(dc, set, (uint32_t)victim);
        line->dirty = true;
        write_through_if_first(dc, cfg, set, (uint32_t)victim, addr);
    }
    return 0;
}

/*
 * External invalidate request (opcode 3 model):
 * - if line exists locally, flush if dirty/M then invalidate it
 * Returns 1 when a local line was invalidated, 0 otherwise.
 */
int cache_data_invalidate(cache_t *dc, sim_cfg_t cfg, uint32_t addr)
{
    uint32_t set = addr_set(addr);
    uint16_t tag = addr_tag(addr);
    int way = find_hit_way(dc, set, tag);
    cache_line_t *line;
    if (way < 0)
    {
        return 0;
    }

    line = cache_line_at(dc, set, (uint32_t)way);
    /* If modified/dirty, flush data before invalidation. */
    if (line->dirty || line->mesi == MESI_M)
    {
        maybe_log(cfg, "Write to L2", line_addr(line->tag, set));
    }
    /* Drop local copy. */
    line->valid = false;
    line->dirty = false;
    line->first_write_done = false;
    line->mesi = MESI_I;
    line->lru = 7;
    return 1;
}

/*
 * External snoop/read request (opcode 4 model):
 * - if line exists locally and is dirty/M, return data to L2
 * - invalidate local copy afterwards
 * Returns 1 on snoop hit, 0 on miss.
 */
int cache_data_snoop_request(cache_t *dc, sim_cfg_t cfg, uint32_t addr) 
{
    uint32_t set = addr_set(addr);
    uint16_t tag = addr_tag(addr);
    int way = find_hit_way(dc, set, tag);
    cache_line_t *line;
    if (way < 0) 
    {
        return 0;
    }

    line = cache_line_at(dc, set, (uint32_t)way);
    /* On snoop hit, provide data if modified/dirty, then invalidate locally. */
    if (line->dirty || line->mesi == MESI_M) 
    {
        maybe_log(cfg, "Return data to L2", line_addr(line->tag, set));
    }
    line->valid = false;
    line->dirty = false;
    line->first_write_done = false;
    line->mesi = MESI_I;
    line->lru = 7;
    return 1;
}

/*
 * Print cache contents.
 * Only valid, non-I lines are shown to keep dumps concise.
 */
void cache_print_dump(const cache_t *cache, const sim_cfg_t *cfg) 
{
    uint32_t set;
    uint32_t way;
    char lru_bits[4];

    cache_log_printf(cfg, "\n%s contents (valid lines only):\n", cache->name);
    cache_log_printf(cfg, "  %-8s %-4s %-6s %-4s %-4s %-6s\n", "Set", "Way", "Tag", "MESI", "LRU", "Dirty");
    cache_log_printf(cfg, "  %-8s %-4s %-6s %-4s %-4s %-6s\n", "----", "----", "----", "----", "----", "-----");
    for (set = 0; set < SET_COUNT; ++set) 
    {
        for (way = 0; way < cache->ways; ++way) 
        {
            const cache_line_t *line = cache_line_at_const(cache, set, way);
            /* Dump only active lines. */
            if (!line->valid || line->mesi == MESI_I) 
            {
                continue;
            }
            /* Render 3-bit LRU value as binary string for readability. */
            lru_bits[0] = (line->lru & 0x4) ? '1' : '0';
            lru_bits[1] = (line->lru & 0x2) ? '1' : '0';
            lru_bits[2] = (line->lru & 0x1) ? '1' : '0';
            lru_bits[3] = '\0';
            cache_log_printf(
                cfg,
                "  0x%04" PRIX32 "   %-4" PRIu32 " 0x%03" PRIX16 "   %-4s %-4s %-6u\n",
                set,
                way,
                line->tag,
                mesi_to_str(line->mesi),
                lru_bits,
                line->dirty ? 1U : 0U
            );
        }
    }
    cache_log_printf(cfg, "  %-8s %-4s %-6s %-4s %-4s %-6s\n", "----", "----", "----", "----", "----", "-----");
}

/*
 * Print summary counters and hit-rate percentage for one cache.
 */
void cache_print_stats(const cache_t *cache, const sim_cfg_t *cfg) 
{
    uint64_t accesses = cache->stats.reads + cache->stats.writes;
    double hit_rate = accesses ? ((double)cache->stats.hits / (double)accesses) * 100.0 : 0.0;

    cache_log_printf(cfg, "%s stats:\n", cache->name);
    cache_log_printf(cfg, "  Reads   : %" PRIu64 "\n", cache->stats.reads);
    cache_log_printf(cfg, "  Writes  : %" PRIu64 "\n", cache->stats.writes);
    cache_log_printf(cfg, "  Hits    : %" PRIu64 "\n", cache->stats.hits);
    cache_log_printf(cfg, "  Misses  : %" PRIu64 "\n", cache->stats.misses);
    cache_log_printf(cfg, "  HitRate : %.2f%%\n", hit_rate);
}
