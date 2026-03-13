#include "simulator.h"

#include "cache.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>

/*
 * Print expected command-line shape and mode semantics.
 * mode 0: normal simulation outputs
 * mode 1: mode 0 + explicit L2 communication logs
 */
static void usage(FILE *out, const char *prog) 
{
    fprintf(out, "Usage: %s <trace_file> <mode>\n", prog);
    fprintf(out, "  mode 0: summary + responses to 9\n");
    fprintf(out, "  mode 1: mode 0 + L2 communication logs\n");
}

/*
 * Extract filename from full path for cleaner log headers.
 * Supports both '/' and '\\' path separators.
 */
static const char *base_name(const char *path) 
{
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *base = path;

    if (slash && bslash) 
    {
        base = (slash > bslash) ? slash + 1 : bslash + 1;
    } else if (slash) 
    {
        base = slash + 1;
    } else if (bslash) 
    {
        base = bslash + 1;
    }
    return base;
}

#if DEBUG
/* Convert MESI state to compact one-character code for debug lines. */
static char mesi_char(mesi_state_t s) 
{
    switch (s) 
    {
        case MESI_M:
            return 'M';
        case MESI_E:
            return 'E';
        case MESI_S:
            return 'S';
        case MESI_I:
        default:
            return 'I';
    }
}

/*
 * Debug-only lookup in a specific set.
 * Returns matched way index, otherwise -1.
 */
static int debug_find_way(const cache_t *cache, uint32_t set, uint16_t tag) 
{
    uint32_t way;
    for (way = 0; way < cache->ways; ++way) 
    {
        const cache_line_t *line = &cache->lines[(size_t)set * cache->ways + way];
        if (line->valid && line->tag == tag && line->mesi != MESI_I) 
        {
            return (int)way;
        }
    }
    return -1;
}

/*
 * Debug dump for one accessed set:
 * - shows requested set/tag and matched way
 * - prints each active way as tag, MESI, LRU bits, dirty
 * Keeps per-operation output short but stateful.
 */
static void debug_print_set_snapshot(const sim_cfg_t *cfg, const cache_t *cache, uint32_t addr) 
{
    uint32_t set = (addr >> BYTE_BITS) & SET_MASK;
    uint16_t tag = (uint16_t)(addr >> TAG_SHIFT);
    uint32_t way;
    int hit_way = debug_find_way(cache, set, tag);

    cache_log_printf(
        cfg,
        "[DEBUG] %s set=0x%04" PRIX32 " tag=0x%03" PRIX16 " way=%d |",
        cache->name,
        set,
        tag,
        hit_way
    );

    for (way = 0; way < cache->ways; ++way) 
    {
        const cache_line_t *line = &cache->lines[(size_t)set * cache->ways + way];
        if (!line->valid && line->mesi == MESI_I) 
        {
            continue;
        }
        cache_log_printf(
            cfg,
            " w%" PRIu32 ":%03" PRIX16 "%c%s d%u",
            way,
            line->tag,
            mesi_char(line->mesi),
            (line->lru <= 7) ? (char[4])
            {
                (char)(((line->lru & 0x4) ? '1' : '0')),
                (char)(((line->lru & 0x2) ? '1' : '0')),
                (char)(((line->lru & 0x1) ? '1' : '0')),
                '\0'
            } : "---",
            line->dirty ? 1U : 0U
        );
    }
    cache_log_printf(cfg, "\n");
}

/*
 * Debug event line + cumulative stats at that point in trace.
 * This helps correlate one operation outcome with running totals.
 */
static void debug_trace_event(
    const sim_cfg_t *cfg,
    uint64_t line_no,
    int op,
    uint32_t addr,
    const char *cache_name,
    const char *result,
    const cache_t *icache,
    const cache_t *dcache
)
{
    cache_log_printf(
        cfg,
        "[DEBUG] line=%" PRIu64 " op=%d addr=0x%08" PRIX32 " cache=%s result=%s\n",
        line_no,
        op,
        addr,
        cache_name,
        result
    );
    cache_log_printf(
        cfg,
        "[DEBUG] stats L1I(R=%" PRIu64 ",W=%" PRIu64 ",H=%" PRIu64 ",M=%" PRIu64
        ") L1D(R=%" PRIu64 ",W=%" PRIu64 ",H=%" PRIu64 ",M=%" PRIu64 ")\n",
        icache->stats.reads,
        icache->stats.writes,
        icache->stats.hits,
        icache->stats.misses,
        dcache->stats.reads,
        dcache->stats.writes,
        dcache->stats.hits,
        dcache->stats.misses
    );
}
#endif

int simulator_main(int argc, char **argv) 
{
    const char *trace_path;
    int mode;
    FILE *fp;
    FILE *log_fp;
    const char *trace_base;

    cache_t icache;
    cache_t dcache;
    sim_cfg_t cfg;
    char line[256];
    uint64_t line_no = 0;
    const char *log_path = "outputs\\output_traces.txt";

    /*
     * Expected args:
     * argv[1] = trace file path
     * argv[2] = mode (0 or 1)
     */
    if (argc < 3) 
    {
        usage(stderr, argv[0]);
        return 1;
    }

    trace_path = argv[1];
    mode = atoi(argv[2]);

    trace_base = base_name(trace_path);

    /*
     * All runs append into one shared output log.
     * Keeping append mode preserves historical runs for comparison.
     */
    _mkdir("outputs");
    log_fp = fopen(log_path, "a");
    if (!log_fp) 
    {
        fprintf(stderr, "Failed to open log file: %s\n", log_path);
        return 1;
    }

    cfg.mode = mode;
    cfg.log_fp = log_fp;

    /* Only 0 and 1 are valid modes */
    if (mode < 0 || mode > 1) 
    {
        cache_log_printf(&cfg, "Invalid mode: %d\n", mode);
        usage(log_fp, argv[0]);
        fclose(log_fp);
        return 1;
    }

    fp = fopen(trace_path, "r");
    if (!fp) 
    {
        cache_log_printf(&cfg, "Failed to open trace file: %s\n", trace_path);
        fclose(log_fp);
        return 1;
    }

    cache_log_printf(&cfg, "============================================================\n");
    cache_log_printf(&cfg, "Trace File : %s\n", trace_base);
    cache_log_printf(&cfg, "Mode       : %d\n", mode);
    cache_log_printf(&cfg, "============================================================\n");

    if (!cache_init(&icache, "L1I", 4U)) 
    {
        fclose(fp);
        cache_log_printf(&cfg, "Failed to allocate instruction cache.\n");
        fclose(log_fp);
        return 1;
    }

    if (!cache_init(&dcache, "L1D", 8U)) 
    {
        cache_free(&icache);
        fclose(fp);
        cache_log_printf(&cfg, "Failed to allocate data cache.\n");
        fclose(log_fp);
        return 1;
    }

    /*
     * Main simulation loop:
     * read each trace entry, parse op/address, dispatch to cache handler.
     */
    while (fgets(line, sizeof(line), fp)) 
    {
        int op;
        uint32_t addr;
        int parsed;

        line_no++;
        /* Ignore blank/comment lines in trace. */
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '#') 
        {
            continue;
        }

        /* Parse "<op> <hex_addr>"; op-only lines default addr to zero. */
        parsed = sscanf(line, "%d %" SCNx32, &op, &addr);
        if (parsed < 1) 
        {
            continue;
        }
        if (parsed == 1) 
        {
            addr = 0;
        }

        /*
         * Opcodes :
         * 0=data read, 1=data write, 2=instruction read
         * 3=invalidate request, 4=snoop request
         * 8=reset both caches, 9=print cache dump
         */
        switch (op) 
        {
            case 0:
#if DEBUG
                {
                    int result = cache_data_read(&dcache, cfg, addr);
                    debug_trace_event(&cfg, line_no, op, addr, "L1D", result ? "HIT" : "MISS", &icache, &dcache);
                    debug_print_set_snapshot(&cfg, &dcache, addr);
                }
#else
                cache_data_read(&dcache, cfg, addr);
#endif
                break;
            case 1:
#if DEBUG
                {
                    int result = cache_data_write(&dcache, cfg, addr);
                    debug_trace_event(&cfg, line_no, op, addr, "L1D", result ? "HIT" : "MISS", &icache, &dcache);
                    debug_print_set_snapshot(&cfg, &dcache, addr);
                }
#else
                cache_data_write(&dcache, cfg, addr);
#endif
                break;
            case 2:
#if DEBUG
                {
                    int result = cache_instr_read(&icache, cfg, addr);
                    debug_trace_event(&cfg, line_no, op, addr, "L1I", result ? "HIT" : "MISS", &icache, &dcache);
                    debug_print_set_snapshot(&cfg, &icache, addr);
                }
#else
                cache_instr_read(&icache, cfg, addr);
#endif
                break;
            case 3:
#if DEBUG
                {
                    int result = cache_data_invalidate(&dcache, cfg, addr);
                    debug_trace_event(&cfg, line_no, op, addr, "L1D", result ? "INVALIDATE_HIT" : "INVALIDATE_MISS", &icache, &dcache);
                    debug_print_set_snapshot(&cfg, &dcache, addr);
                }
#else
                cache_data_invalidate(&dcache, cfg, addr);
#endif
                break;
            case 4:
#if DEBUG
                {
                    int result = cache_data_snoop_request(&dcache, cfg, addr);
                    debug_trace_event(&cfg, line_no, op, addr, "L1D", result ? "SNOOP_HIT" : "SNOOP_MISS", &icache, &dcache);
                    debug_print_set_snapshot(&cfg, &dcache, addr);
                }
#else
                cache_data_snoop_request(&dcache, cfg, addr);
#endif
                break;
            case 8:
                /* Reset both caches and their statistics. */
                cache_reset(&icache);
                cache_reset(&dcache);
#if DEBUG
                cache_log_printf(&cfg, "[DEBUG] line=%" PRIu64 " op=%d action=RESET\n", line_no, op);
                debug_trace_event(&cfg, line_no, op, addr, "ALL", "RESET_DONE", &icache, &dcache);
#endif
                break;
            case 9:
                /* Dump current visible valid cache contents. */
                cache_log_printf(&cfg, "\n==== Cache Dump @ Trace Line %" PRIu64 " ====\n", line_no);
                cache_print_dump(&icache, &cfg);
                cache_print_dump(&dcache, &cfg);
                cache_log_printf(&cfg, "==== End Dump ====\n");
#if DEBUG
                debug_trace_event(&cfg, line_no, op, addr, "ALL", "DUMP", &icache, &dcache);
#endif
                break;
            default:
                cache_log_printf(&cfg, "Warning: unknown op %d at line %" PRIu64 "\n", op, line_no);
                break;
        }
    }

    fclose(fp);

    /* Always print end-of-run summary for both caches. */
    cache_log_printf(&cfg, "\n==== Final Summary ====\n");
    cache_print_stats(&icache, &cfg);
    cache_print_stats(&dcache, &cfg);
    cache_log_printf(&cfg, "============================================================\n\n");

    cache_free(&icache);
    cache_free(&dcache);

    fclose(log_fp);
    return 0;
}
