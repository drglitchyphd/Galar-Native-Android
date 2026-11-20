// The unified driver layer's quirk table (nvn/vk_quirks.h) against the per-site vendor tests it replaced in nvn_vk.c, copied here as they were: over a grid of GPU
// descriptions (vendors, driver IDs or none, Arm releases r20p0-r55p2 with and without beta, driver packages, device names, memory sizes, dynamic rendering) every
// default must come out the same, so moving the decisions into one table changed no phone's behaviour. Then the phones from players' logs, by name.
// cc -O2 -I../nvn -o /tmp/t_quirks t_quirks.c && /tmp/t_quirks
#include <stdio.h>
#include <string.h>
#include "vk_quirks.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { if (fails++ < 20) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// ---- the old expressions, as they stood in nvn_vk.c before vk_quirks.h (g_props.* → g->*, g_arm_* → g->arm_*, getenv("DM_VK_DRIVER") → g->custom) ----
static int old_arm_driver(const gpu_id *g) { return g->driver_id >= 0 ? g->driver_id == 9 : g->vendor == 0x13B5; }   // drvp ? dp.driverID == ARM : vendor == 0x13B5
static int old_sec_driver(const gpu_id *g) { return g->driver_id >= 0 ? g->driver_id == 21 : g->vendor == 0x144D; }
static int old(int q, const gpu_id *g) {
    int v = g->arm_rel * 100 + g->arm_p, arc = g->port == PORT_ARCEUS;
    // Arceus Native's own expressions (Arceus-Native-Android main 3c5c6d1, nvn_vk.c), where they differ from Galar's
    if (arc) switch (q) {
    case Q_NO_DYNREN: return 0;                               // g_dynren = dynren && !(DM_VK_NO_DYNREN): no Adreno default
    case Q_ASYNC_PSO: return 0;                               // v = e ? atoi(e) != 0 : 0 (#34)
    case Q_NO_EDS: return g->vendor == 0x5143 && ((g->api >> 12) & 0x3ff) < 2 && ((g->api >> 22) & 0x7f) == 1;   // old_qc
    case Q_BC_GPU_OFF: return g->vendor == 0x1010;            // "BC layer: off on PowerVR"
    }
    switch (q) {
    case Q_NO_EDS: case Q_BC_GPU_OFF: return 0;               // (not in Galar)
    case Q_NO_DYNREN: return g->vendor == 0x5143;
    case Q_HOST_IMPORT_OFF: return g->vendor == 0x144D;
    case Q_DMAHEAP: return g->vendor == 0x13B5 || g->vendor == 0x144D;
    case Q_DMABUF_TRY: return g->vendor == 0x13B5;
    case Q_G615_SAFE: return g->vendor == 0x13B5 && (g->driver_version >> 22) < 49 && strstr(g->name, "G615") && !g->custom;
    case Q_FLOAT_CONTROLS: return old_arm_driver(g) || old_sec_driver(g);
    case Q_CLAMP_ZERO: return g->arm_rel && ((v >= 3801 && v < 4901) || (g->arm_beta && v >= 4901 && v < 4902) || (v >= 5000 && v < 5101));
    case Q_NAN_MINMAX: return g->arm_rel ? 2 : 0;
    case Q_EXACT_MATH: return g->arm_rel ? 1 : 0;            // (EXACT_ALL at the site either way)
    case Q_INIT_LOCALS: return g->arm_rel != 0;
    case Q_VIEW_QUARANTINE: return g->arm_rel && g->dynren ? 2048 : 0;
    case Q_TEX_HALF_UNSAFE: return g->arm_rel && g->arm_rel < 49;
    case Q_ARM_FORMATS: return g->vendor == 0x13B5;
    case Q_BARRIER_RELAXED: return g->vendor == 0x5143;      // (|| arceus_pass_default at the site, unchanged)
    case Q_GATHER_BIAS: return g->vendor == 0x5143;
    case Q_PASS_DEFER: return g->vendor == 0x5143;
    case Q_ASYNC_PSO: return g->vendor == 0x5143;
    case Q_PIPE_KEEP: return (g->vendor == 0x13B5 || (g->ram_kb > 0 && g->ram_kb < 7500000)) ? 800 : 0;
    case Q_CLEAR_BY_PASS: return g->vendor == 0x144D;
    case Q_PCACHE_GAPS_OFF: return g->vendor == 0x1010;
    }
    return -999;
}
// Arm's release as nvn_vk.c reads it: only for Arm's own driver (by driver ID; without driver properties, vendor 0x13B5 and not a package)
static void arm_fields(gpu_id *g, int rel, int p, int beta) {
    int arm = g->driver_id >= 0 ? g->driver_id == 9 : (g->vendor == 0x13B5 && !g->custom);
    g->arm_rel = arm ? rel : 0; g->arm_p = arm ? p : 0; g->arm_beta = arm && g->driver_id >= 0 ? beta : 0;
    g->driver_version = (uint32_t)rel << 22 | (uint32_t)p << 12;
}
static gpu_id mk_(int port, uint32_t api, uint32_t ven, int did, int rel, int p, int beta, int custom, const char *name, long ram, int dynren) {
    gpu_id g; memset(&g, 0, sizeof g); g.port = port; g.api = api; g.vendor = ven; g.driver_id = did; g.custom = custom; g.ram_kb = ram; g.dynren = dynren; snprintf(g.name, sizeof g.name, "%s", name);
    arm_fields(&g, rel, p, beta); return g;
}
#define mk(...) mk_(PORT_GALAR, 1u << 22 | 3u << 12, __VA_ARGS__)
int main(void) {
    const uint32_t vens[] = { 0x5143, 0x13B5, 0x144D, 0x1010, 0x10DE, 0x1002, 0 };
    const int dids[] = { -1, 9, 21, 8 /* Qualcomm */, 18 /* Turnip */, 20 /* PanVK */, 0 };
    const char *names[] = { "Mali-G615 MC2", "Mali-G720-Immortalis MC12", "Adreno (TM) 830", "Samsung Xclipse 940", "PowerVR B-Series BXM-8-256", "" };
    const long rams[] = { 0, 3800000, 7499999, 7500000, 11800000 };
    const uint32_t apis[] = { 1u << 22, 1u << 22 | 1u << 12 | 128, 1u << 22 | 2u << 12, 1u << 22 | 3u << 12 | 231 };
    int n = 0;
    for (int port = 0; port <= 1; port++) for (unsigned ap = 0; ap < 4; ap++)
    for (unsigned a = 0; a < sizeof vens / sizeof *vens; a++) for (unsigned b = 0; b < sizeof dids / sizeof *dids; b++)
    for (int rel = 20; rel <= 55; rel += 1) for (int p = 0; p <= 2; p++) for (int beta = 0; beta <= 1; beta++) for (int custom = 0; custom <= 1; custom++) {
        if (vens[a] != 0x13B5 && (rel != 20 || p || beta)) continue;   // (the release only matters for Arm's ID)
        for (unsigned c = 0; c < sizeof names / sizeof *names; c++) for (unsigned d = 0; d < sizeof rams / sizeof *rams; d++) for (int dr = 0; dr <= 1; dr++) {
            gpu_id g = mk_(port, apis[ap], vens[a], dids[b], rel, p, beta, custom, names[c], rams[d], dr); n++;
            for (int q = 0; q < Q_N; q++) CHECK(quirk_default(q, &g) == old(q, &g), "port %d api %#x quirk %d (%s) on vendor %#x driver %d r%dp%d%s%s \"%s\" %ld kB dynren %d: table %d, old %d", g.port, g.api, q, Q_INFO[q].what,
                g.vendor, g.driver_id, rel, p, beta ? " beta" : "", custom ? " package" : "", g.name, g.ram_kb, dr, quirk_default(q, &g), old(q, &g));
        }
    }
    printf("grid: %d GPU descriptions x %d quirks = %d comparisons\n", n, Q_N, checks);
    // ---- phones from players' logs ----
    gpu_id s25 = mk(0x5143, 8, 0, 0, 0, 0, "Adreno (TM) 830", 11800000, 0);
    CHECK(quirk_default(Q_NO_DYNREN, &s25) && quirk_default(Q_ASYNC_PSO, &s25) && quirk_default(Q_GATHER_BIAS, &s25) && !quirk_default(Q_FLOAT_CONTROLS, &s25) && !quirk_default(Q_PIPE_KEEP, &s25),
          "S25+ (Adreno 830, Qualcomm): render passes, async pipelines, gather bias; no Mali or Xclipse workarounds");
    gpu_id g615 = mk(0x13B5, 9, 44, 1, 0, 0, "Mali-G615 MC2", 7800000, 1);
    CHECK(quirk_default(Q_G615_SAFE, &g615) && quirk_default(Q_CLAMP_ZERO, &g615) && quirk_default(Q_NAN_MINMAX, &g615) == 2 && quirk_default(Q_VIEW_QUARANTINE, &g615) == 2048
          && quirk_default(Q_TEX_HALF_UNSAFE, &g615) && quirk_default(Q_PIPE_KEEP, &g615) == 800 && !quirk_default(Q_NO_DYNREN, &g615), "Mali-G615 MC2 on r44p1 (#82 #110): the full Arm set");
    gpu_id g925 = mk(0x13B5, 9, 49, 1, 1, 0, "Mali-G925-Immortalis MC12", 11800000, 1);
    CHECK(quirk_default(Q_CLAMP_ZERO, &g925) && !quirk_default(Q_G615_SAFE, &g925) && !quirk_default(Q_TEX_HALF_UNSAFE, &g925), "Mali-G925 beta r49p1 (#111): erratum workaround (beta), no G615 set");
    gpu_id g925r = mk(0x13B5, 9, 49, 1, 0, 0, "Mali-G925-Immortalis MC12", 11800000, 1);
    CHECK(!quirk_default(Q_CLAMP_ZERO, &g925r), "Mali r49p1 release: the erratum fixed");
    gpu_id panvk = mk(0x13B5, 20, 25, 0, 0, 1, "Mali-G615 MC6 (Panfrost)", 7800000, 1);
    CHECK(!quirk_default(Q_FLOAT_CONTROLS, &panvk) && !quirk_default(Q_NAN_MINMAX, &panvk) && !quirk_default(Q_G615_SAFE, &panvk) && quirk_default(Q_ARM_FORMATS, &panvk),
          "PanVK on a Mali-G615: none of Arm's driver workarounds, Mali's format lists kept");
    gpu_id xcl = mk(0x144D, 21, 0, 0, 0, 0, "Samsung Xclipse 940", 7800000, 1);
    CHECK(quirk_default(Q_HOST_IMPORT_OFF, &xcl) && quirk_default(Q_DMAHEAP, &xcl) && quirk_default(Q_FLOAT_CONTROLS, &xcl) && quirk_default(Q_CLEAR_BY_PASS, &xcl), "Xclipse 940 (#39 #41): Samsung's set");
    gpu_id pvr = mk(0x1010, 0, 0, 0, 0, 0, "PowerVR B-Series BXM-8-256", 3800000, 0);
    CHECK(quirk_default(Q_PCACHE_GAPS_OFF, &pvr) && quirk_default(Q_PIPE_KEEP, &pvr) == 800, "PowerVR BXM-8-256 on 4 GB: no cache for gap pipelines, pipelines capped");
    gpu_id a710 = mk_(PORT_ARCEUS, 1u << 22 | 1u << 12 | 128, 0x5143, 8, 0, 0, 0, 0, "Adreno (TM) 710", 5600000, 1);
    CHECK(quirk_default(Q_NO_EDS, &a710) && !quirk_default(Q_ASYNC_PSO, &a710) && !quirk_default(Q_NO_DYNREN, &a710), "Arceus on an Adreno 710 with Qualcomm's 1.1 driver (#34): strides in pipelines, no async pipelines, dynamic rendering");
    gpu_id a710g = mk_(PORT_GALAR, 1u << 22 | 1u << 12 | 128, 0x5143, 8, 0, 0, 0, 0, "Adreno (TM) 710", 5600000, 1);
    CHECK(!quirk_default(Q_NO_EDS, &a710g) && quirk_default(Q_ASYNC_PSO, &a710g) && quirk_default(Q_NO_DYNREN, &a710g), "Galar on the same Adreno 710: Galar's rules, unchanged");
    gpu_id g5 = mk_(PORT_ARCEUS, 1u << 22 | 3u << 12, 0x1010, 0, 0, 0, 0, 0, "PowerVR D-Series DXT-48-1536", 11800000, 1);
    CHECK(quirk_default(Q_BC_GPU_OFF, &g5), "Arceus on a Tensor G5 (PowerVR, #32): BC on the CPU");
    printf(fails ? "%d of %d FAILED\n" : "all %d passed\n", fails ? fails : checks, checks);
    return fails != 0;
}
