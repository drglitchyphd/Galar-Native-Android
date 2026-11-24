// The prewarm's rules that need no GPU (nvn_vk.c prewarm_*), kept apart so tests/t_prewarm.c can run them on a PC (perf-1124 fable-loads.md S3 / S4):
//   - the stamp: what a complete prewarm leaves beside the pipeline cache (<DM_VK_PCACHE>.stamp), so the next start knows every listed pipeline is in the
//     driver's cache and need not hold the game (S3). Any change of the lists, the cache, the driver, the app, the shader set or the shader rewrites differs;
//   - the hold: DM_VK_PREWARM_HOLD=all | none | <count> (the first <count> list entries);
//   - the list compaction: a pipeline list without its repeats, the first of each kept in place (S4: the record list only ever grew on phones under the cap);
//   - the cap: how many of the list are kept built where the driver's pipelines are capped (Q_PIPE_KEEP), the rest only put into the driver's cache.
// The record format and the shader-chain IDs are not touched: the stamp is a file of its own, the compaction writes the same entries in the same format.
#ifndef PREWARM_POLICY_H
#define PREWARM_POLICY_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hmap.h"

typedef struct {
    uint64_t seed, rec, rec_sst, cs_seed, cs_rec;   // the lists' file sizes (shipped, recorded here, its draw-state list, the compute lists)
    uint64_t cache;                                 // the pipeline cache's bytes as the driver took them / as written
    uint64_t pak, pak_mtime;                        // the shader pack's size and time (the setup made it again: other SPIR-V)
    uint32_t vendor, device, drv_ver, flags;        // the GPU, its driver version, the shader rewrites in force (nvn_vk.c pw_flags)
    uint8_t uuid[16];                               // the driver's pipelineCacheUUID
    char app[32], chain[24];                        // the app's version code (DM_APP_VERSION), the shader chain ID
} pw_stamp;

static inline int pw_stamp_fmt(const pw_stamp *s, char *out, size_t n) {
    char u[33]; for (int i = 0; i < 16; i++) snprintf(u + 2 * i, 3, "%02x", s->uuid[i]);
    return snprintf(out, n, "prewarm-stamp 1 seed %llu rec %llu sst %llu cs %llu/%llu cache %llu pak %llu/%llu gpu %08x/%08x driver %08x flags %08x uuid %s app %s chain %s\n",
                    (unsigned long long)s->seed, (unsigned long long)s->rec, (unsigned long long)s->rec_sst, (unsigned long long)s->cs_seed, (unsigned long long)s->cs_rec,
                    (unsigned long long)s->cache, (unsigned long long)s->pak, (unsigned long long)s->pak_mtime, s->vendor, s->device, s->drv_ver, s->flags, u,
                    s->app[0] ? s->app : "-", s->chain[0] ? s->chain : "-");
}
// 1: the file's text is the stamp of s (a list grown, the cache rewritten or refused, another driver, app or shader set: 0)
static inline int pw_stamp_match(const char *text, const pw_stamp *s) {
    char want[512]; pw_stamp_fmt(s, want, sizeof want);
    return text && s->cache > 0 && !strcmp(text, want);
}

enum { PW_HOLD_ALL = -1, PW_HOLD_NONE = 0 };
// DM_VK_PREWARM_HOLD: "all" (every listed pipeline before the game, as before), "none", or a count (the first <count> list entries); else dflt
static inline int pw_hold_parse(const char *e, int dflt) {
    if (!e || !*e) return dflt;
    if (!strcmp(e, "all")) return PW_HOLD_ALL;
    if (!strcmp(e, "none") || !strcmp(e, "0")) return PW_HOLD_NONE;
    char *x; long v = strtol(e, &x, 10); if (*x || v <= 0) return dflt;
    return v > 1000000 ? 1000000 : (int)v;
}

// How many of the list to keep built under a cap of `cap` live pipelines (0: no cap): 90 %, the eviction's own target (pso_lru.h)
static inline int pw_keep_of_cap(int cap) { return cap > 0 ? cap * 9 / 10 : 0; }

// The list compaction: e[0..n) entries of esz bytes; repeats (and entries found in `skip`, the shipped list's) dropped, the first of each kept in its place.
//   Returns the new count; e is compacted in place
static inline int pw_dedupe(uint8_t *e, int n, size_t esz, const nvn_hmap *skip) {
    nvn_hmap seen; nvn_hm_init(&seen, (uint32_t)esz); int m = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *x = e + (size_t)i * esz;
        if ((skip && nvn_hm_get(skip, x)) || nvn_hm_get(&seen, x)) continue;
        if (m != i) memmove(e + (size_t)m * esz, x, esz);
        nvn_hm_put(&seen, e + (size_t)m * esz, (void *)1); m++;   // (the key copied into the map: e moves under it)
    }
    free(seen.keys); free(seen.vals);
    return m;
}
// Rewritten only when the repeats are more than a tenth of the list (a few are normal: a run that ended before its writes did)
static inline int pw_compact_worth(int n, int kept) { return n > 0 && (n - kept) * 10 > n; }
#endif
