// Memory tier at start (fix 6 of the 2026-11-24 memory analysis): the textures' default from what the phone has free when the game starts, not only from its total RAM.
// The POCO F6 (7,293 MB) had 1.4-1.7 GB available at start, got full textures from its chip and RAM tier (GameSettings.java calls 7.5 GB "mid"), and swapped 450-840 MB
// in play. Under 2.5 GB available on a phone under 9 GB in all: half textures by default (DM_VK_TEX_QUALITY=half with DM_VK_TEX_QUALITY_AUTO, so nvn_vk.c still keeps
// full on Arm's drivers before r49, Q_TEX_HALF_UNSAFE). The player's own Textures setting always wins (env.txt is read first); DM_MEM_TIER=0 turns this off.
// 12 GB phones (about 11.2 M kB in MemTotal) never get it: their free memory at start is the system's choice to keep apps cached, not a shortage.
// Test: tests/t_mem_tier.c
#ifndef MEM_TIER_H
#define MEM_TIER_H
#include <stdio.h>

enum { MEM_TIER_AVAIL_KB = 2560 * 1024, MEM_TIER_TOTAL_KB = 9000000 };   // 2.5 GB available; 9 GB in all (an "8 GB" phone reports 7.3-7.6 M kB)

// 1: default to half textures. Unknown values (0 or less: /proc/meminfo unreadable) decide nothing
static int mem_tier_half(long avail_kb, long total_kb) {
    return avail_kb > 0 && total_kb > 0 && avail_kb < MEM_TIER_AVAIL_KB && total_kb < MEM_TIER_TOTAL_KB;
}
// MemAvailable and MemTotal from a meminfo file (/proc/meminfo); 0 for a value not found. Returns 1 when both were read
static int mem_tier_read(const char *path, long *avail_kb, long *total_kb) {
    *avail_kb = *total_kb = 0; FILE *f = fopen(path, "r"); char ln[128];
    while (f && fgets(ln, sizeof ln, f)) { long v; if (sscanf(ln, "MemAvailable: %ld kB", &v) == 1) *avail_kb = v; else if (sscanf(ln, "MemTotal: %ld kB", &v) == 1) *total_kb = v; }
    if (f) fclose(f);
    return *avail_kb > 0 && *total_kb > 0;
}
#endif
