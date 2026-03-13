#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Compile-time debug switch (0 = off, 1 = on). */
#ifndef DEBUG
#define DEBUG 0
#endif

#define LINE_SIZE 64U
#define SET_COUNT 16384U
#define INDEX_BITS 14U
#define BYTE_BITS 6U
/* Tag starts above set+byte-offset bits */
#define TAG_SHIFT (INDEX_BITS + BYTE_BITS)
/* Mask to extract set index from shifted address */
#define SET_MASK (SET_COUNT - 1U)

/* Per-line coherence state */
typedef enum 
{
    MESI_I = 0,
    MESI_S = 1,
    MESI_E = 2,
    MESI_M = 3
} mesi_state_t;

/* One cache line entry stored in a set/way slot. */
typedef struct 
{
    bool valid;
    bool dirty;
    /* Used by first-write logging behavior. */
    bool first_write_done;
    /* 3-bit LRU rank stored in 16-bit field for simplicity. */
    uint16_t lru;
    uint16_t tag;
    mesi_state_t mesi;
} cache_line_t;

/* Running counters for summary reporting. */
typedef struct 
{
    uint64_t reads;
    uint64_t writes;
    uint64_t hits;
    uint64_t misses;
} cache_stats_t;

/* Cache instance (L1I or L1D) */
typedef struct 
{
    const char *name;
    uint32_t ways;
    /* Flat array of SET_COUNT * ways lines */
    cache_line_t *lines;
    cache_stats_t stats;
} cache_t;

/* Shared runtime config passed to cache operations */
typedef struct 
{
    /* mode 0: minimal output, mode 1: include L2 traffic logs */
    int mode;
    /* append log file used by cache_log_printf */
    FILE *log_fp;
} sim_cfg_t;

/* Lifecycle helpers */
int cache_init(cache_t *cache, const char *name, uint32_t ways);
void cache_free(cache_t *cache);
void cache_reset(cache_t *cache);

/* Trace opcode handlers. Return 1 on hit/handled-hit, 0 on miss */
int cache_data_read(cache_t *dc, sim_cfg_t cfg, uint32_t addr);
int cache_data_write(cache_t *dc, sim_cfg_t cfg, uint32_t addr);
int cache_instr_read(cache_t *ic, sim_cfg_t cfg, uint32_t addr);
int cache_data_invalidate(cache_t *dc, sim_cfg_t cfg, uint32_t addr);
int cache_data_snoop_request(cache_t *dc, sim_cfg_t cfg, uint32_t addr);

/* reporting helpers. */
void cache_print_dump(const cache_t *cache, const sim_cfg_t *cfg);
void cache_print_stats(const cache_t *cache, const sim_cfg_t *cfg);

/* Console + file logger */
void cache_log_printf(const sim_cfg_t *cfg, const char *fmt, ...);

#endif
