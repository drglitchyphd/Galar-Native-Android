// The unified driver layer's quirk table (nvn/vk_quirks.h) against the per-site rules it replaced, as beta-exit decided them (the reviewed behaviour of all three
// apps, docs/ENGINE_UNIFY.md), copied below as they stood there, for each port with its own profile (game.tsv port and engine_quirks): every rule as beta-exit
// 6e0e1dd had it (each of those sites is the same line at beta-exit's head 3d21471), and Q_ASYNC_PSO as the head has it (f09e58f, the owner's call: pipelines the
// lists lacked built in the background on every GPU, not only on Adreno; 6e0e1dd's rule stays below as ref_6e0e1dd_async_pso, and part 1 checks where they differ),
// and Q_B10_STANDIN / Q_MALI_WHITE_CHECK as mali-all e6f6d0c decided them (Sword / Shield's white-screen fix: mali_white.h mw_default_route / mw_enabled):
//   1. every quirk's default for Galar Native (Sword / Shield), Arceus Native and Paldea Native (Scarlet / Violet), over a grid of GPU descriptions (vendors, driver
//      IDs or none, Vulkan 1.0-1.4, driver versions with Arm releases r20p0-r55p2 with and without beta, driver packages, device names, memory sizes, dynamic
//      rendering): what the reference site decided, so moving the decisions into one table changed no phone's behaviour in any game. Q_QUERY_LOD is the one new
//      rule; background pipelines differ from 6e0e1dd exactly on the GPUs other than Adreno, in the games without async_pso_off; the white-screen fix's two rules
//      are e6f6d0c's with the game read from the profile's mali_lit_standin, and against e6f6d0c's own test of the port (DM_PORT_GALAR) for the real profiles;
//   2. no rule reads the port: each port with each combination of the engine_quirks bits gets the rules of a game whose profile says those quirks;
//   3. the three profiles as dm/games/<id>/game.tsv has them (port, engine_quirks; Sword and Scarlet through tables_from), so a profile change shows up here;
//   4. the phones from players' logs, by name, in each game;
//   5. the rules answered before gpu_id is complete (quirk_late: nvn_vk_quirk_default's -1 window), and the ones the probe asks (Q_FLOAT_CONTROLS, Q_CLEAR_BY_PASS)
//      from the vendor and driver ID alone;
//   6. where a setting came from: the player's env.txt or the driver check (DM_DRIVER_CHECK_KEYS), and quirk_note keeping a rule's first decision.
// cc -O2 -Wall -I../nvn -o /tmp/t_quirks t_quirks.c && /tmp/t_quirks [games folder, default ../../games]        (or: make t_quirks GAME=<any id> && ./t_quirks)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vk_quirks.h"

static long fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { if (fails++ < 25) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)
#define VK_API_VERSION_MAJOR(v) ((uint32_t)(v) >> 22U & 0x7FU)
#define VK_API_VERSION_MINOR(v) ((uint32_t)(v) >> 12U & 0x3FFU)

// ---- the ports as their profiles build them: game.tsv port → DM_PORT_*, engine_quirks → DM_GAME_ENG_* (dm/games/game.py ENGINE_QUIRKS; part 3 reads the files) ----
typedef struct { const char *ids, *app; int port; int qc11_no_eds, gpu_time_auto_only, frame_hold_off, async_pso_off, framegen_game_rate, nvn_async_never, driver_selftests,
                 mali_lit_standin; } profile;
static const profile PROFILES[] = {
    { "01008DB008C2C000 0100ABF008968000", "Galar Native", PORT_GALAR, 0, 0, 0, 0, 0, 0, 0, 1 },    // Pokémon Shield, Sword (tables_from Shield): mali_lit_standin
    { "01001F5010DFA000", "Arceus Native", PORT_ARCEUS, 1, 1, 1, 1, 1, 1, 1, 0 },                   // Pokémon Legends: Arceus: the first seven
    { "01008F6008C5E000 0100A3D008C5C000", "Paldea Native", PORT_PALDEA, 0, 0, 0, 0, 0, 0, 0, 0 },  // Pokémon Violet, Scarlet (tables_from Violet)
};
enum { NPROF = sizeof PROFILES / sizeof *PROFILES };
// gpu_id.eng as nvn_vk.c mtl_init packs it from game_profile.h's DM_GAME_ENG_*, each bit by name
static unsigned eng_of(const profile *P) {
    return (P->qc11_no_eds ? ENG_QC11_NO_EDS : 0u) | (P->async_pso_off ? ENG_ASYNC_PSO_OFF : 0u) | (P->driver_selftests ? ENG_DRIVER_SELFTESTS : 0u) | (P->nvn_async_never ? ENG_NVN_ASYNC_NEVER : 0u)
         | (P->mali_lit_standin ? ENG_MALI_LIT_STANDIN : 0u);
}

// ---- beta-exit's per-site defaults (env.txt unset), copied (file:line in 6e0e1dd, nvn_vk.c unless named; a line beta-exit changed since is named with the revision
//   it was copied from: Q_ASYNC_PSO, the head 3d21471). g_props.vendorID → g->vendor, .driverVersion → g->driver_version, .deviceName → g->name, .apiVersion →
//   g->api; g_arm_rel / g_arm_p / g_arm_beta → g->arm_*; g_dynren → g->dynren; getenv ("DM_VK_DRIVER") set → g->custom; MemTotal → g->ram_kb; DM_GAME_ENG_<X> →
//   P-><x>; with driver properties (drvp) dp.driverID → g->driver_id, without them g->driver_id is -1 ----
static int old_qcom_driver(const gpu_id *g) { return g->vendor == 0x5143 && VK_API_VERSION_MINOR(g->api) < 3 && !strncmp(g->name, "Adreno", 6); }   // :1594
// mali-all e6f6d0c mali_white.h:174-181, copied (MW_ROUTE_A is 1, MW_ROUTE_NONE 0)
static int ref_mw_enabled(int arm_own, int galar, const char *ladder_env, const char *sim_env) {
    int l = ladder_env && *ladder_env ? atoi(ladder_env) : -1, s = sim_env && *sim_env ? atoi(sim_env) : 0;
    if (l == 0) return 0;
    if (l > 0 || s > 0) return 2;
    return MALI_LIT_DEFAULT && arm_own && galar ? 1 : 0;   // v0.96: off by default (vk_quirks.h MALI_LIT_DEFAULT)
}
static int ref_mw_default_route(int arm_own, int galar) { return MALI_LIT_DEFAULT && arm_own && galar ? 1 : 0; }
static int ref(int q, const gpu_id *g, const profile *P) {
    int v = g->arm_rel * 100 + g->arm_p;
    int arm = g->driver_id >= 0 ? g->driver_id == 9 : g->vendor == 0x13B5;     // :599 arm = drvp ? dp.driverID == VK_DRIVER_ID_ARM_PROPRIETARY : g_props.vendorID == 0x13B5
    int sec = g->driver_id >= 0 ? g->driver_id == 21 : g->vendor == 0x144D;    // :600 sec = drvp ? dp.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY : g_props.vendorID == 0x144D
    switch (q) {
    case Q_HOST_IMPORT_OFF: return g->vendor == 0x144D;                         // :418  why = g_props.vendorID == 0x144D ? "Samsung Xclipse" : NULL
    case Q_IMPORT_MARK_OFF: return !(g->vendor != 0x5143);                      // :420  if (!why && g_props.vendorID != 0x5143 && host_import_mark (0)) (the marker not read on Adreno)
    case Q_DMAHEAP: return g->vendor == 0x13B5 || g->vendor == 0x144D;          // :465  want = he ? atoi (he) : (!g_fdmap && !heap_crashed && (g_props.vendorID == 0x13B5 || ... == 0x144D))
    case Q_DMABUF_TRY: return g->vendor == 0x13B5;                              // :482  if (!g_fdmap && !g_heapmap && !fd_crashed && dmabuf && g_props.vendorID == 0x13B5)
    case Q_NO_DYNREN: return g->vendor == 0x5143;                               // :528  no = e ? atoi (e) != 0 : g_props.vendorID == 0x5143
    case Q_NO_EDS: return P->qc11_no_eds && g->vendor == 0x5143 && VK_API_VERSION_MINOR(g->api) < 2 && VK_API_VERSION_MAJOR(g->api) == 1;   // :534 old_qc
    case Q_G615_SAFE: return g->vendor == 0x13B5 && (g->driver_version >> 22) < 49 && strstr(g->name, "G615") && !g->custom;   // :546
    case Q_BC_DECODE: return g->vendor == 0x1010;                               // :560  if (g_props.vendorID == 0x1010) { ... if (!tc) setenv ("DM_VK_BC_TRANSCODE", "off", 0)
    case Q_FLOAT_CONTROLS: return arm || sec;                                   // :602  g_fctl = fc.shaderSignedZeroInfNanPreserveFloat32 && (e ? atoi (e) != 0 : arm || sec)
    case Q_CLAMP_ZERO: return g->arm_rel && ((v >= 3801 && v < 4901) || (g->arm_beta && v >= 4901 && v < 4902) || (v >= 5000 && v < 5101));   // :633
    case Q_NAN_MINMAX: return g->arm_rel ? 2 : 0;                               // :664  g_nanmm = e ? atoi (e) : g_arm_rel ? 2 : 0
    case Q_EXACT_MATH: return g->arm_rel ? 1 : 0;                               // :671  g_exact = e ? ... : g_arm_rel ? EXACT_ALL : 0 (1 here: the site maps it to EXACT_ALL)
    case Q_INIT_LOCALS: return g->arm_rel != 0;                                 // :676  g_initloc = e ? atoi (e) != 0 : g_arm_rel != 0
    case Q_BC_GPU_OFF: return g->vendor == 0x1010;                              // :723  else if (g_props.vendorID == 0x1010 && !(be && atoi (be) > 0)) "BC layer: off on PowerVR"
    case Q_DRIVER_SELFTESTS: return P->driver_selftests;                        // :746  #if DM_GAME_ENG_DRIVER_SELFTESTS  vtx_canary (); tf_selftest (); #endif
    case Q_VIEW_QUARANTINE: return g->arm_rel && g->dynren ? 2048 : 0;          // :859  g_vqcap = e ? atoi (e) : g_arm_rel && g_dynren ? 2048 : 0
    case Q_GPU_TIME_OFF: return !(g->vendor != 0x13B5 && g->vendor != 0x1010);  // :1458 want_ts = gt ? atoi (gt) != 0 : (g_props.vendorID != 0x13B5 && g_props.vendorID != 0x1010)
    case Q_BARRIER_RELAXED: return g->vendor == 0x5143 && !old_qcom_driver(g);  // :1596 v = e && (full || relaxed) ? !strcmp (e, "relaxed") : g_props.vendorID == 0x5143 && !old_qcom_driver ()
    case Q_TEX_HALF_UNSAFE: return g->arm_rel && g->arm_rel < 49;               // :2233 if (v && g_arm_rel && g_arm_rel < 49 && getenv ("DM_VK_TEX_QUALITY_AUTO")) v = 0
    case Q_ARM_FORMATS: return g->vendor == 0x13B5;                             // :2279 arm = g_props.vendorID == 0x13B5 || getenv ("DM_VK_ARM_FORMATS") != NULL
    case Q_GATHER_BIAS: return g->vendor == 0x5143;                             // :3072 gbias = e ? atoi (e) != 0 : g_props.vendorID == 0x5143
    case Q_PASS_DEFER: return g->vendor == 0x5143;                              // :3573 v = e ? atoi (e) != 0 : g_props.vendorID == 0x5143
    case Q_PIPE_KEEP: return (g->vendor == 0x13B5 || (g->ram_kb > 0 && g->ram_kb < 7500000)) ? 800 : 0;   // :3850 cap = e ? atoi (e) : (... || (kb > 0 && kb < 7500000)) ? 800 : 0
    case Q_ASYNC_PSO: return !P->async_pso_off;                                 // 3d21471 nvn_vk.c:3964 v = e ? atoi (e) != 0 : !DM_GAME_ENG_ASYNC_PSO_OFF (f09e58f: every GPU)
    case Q_PCACHE_GAPS_OFF: return g->vendor == 0x1010;                         // :4194 if (g_props.vendorID == 0x1010 && !g_dynren) ... pc_ = VK_NULL_HANDLE (!g_dynren stays at the site)
    case Q_TF_CHECK: return g->arm_rel > 0;                                     // :4688 chk = e ? atoi (e) != 0 : g_arm_rel > 0
    case Q_CLEAR_BY_PASS: return g->vendor == 0x144D;                           // :4947 v = e ? atoi (e) != 0 : g_props.vendorID == 0x144D
    case Q_TF_SELFTEST: return g->arm_rel > 0;                                  // tf_selftest.h:108 run = on ? atoi (on) != 0 : g_arm_rel > 0 (within :746, Q_DRIVER_SELFTESTS)
    case Q_NVN_ASYNC: return P->nvn_async_never ? 0 : strstr(g->name, "Adreno") != NULL;   // hle_nvn.c:1083 autoon = !(e && *e) && g && strstr (g, "Adreno"); :1085 #if DM_GAME_ENG_NVN_ASYNC_NEVER v = 0
    // new with the merged branches, not in 6e0e1dd (docs/ENGINE_UNIFY.md lists it): galar-driver-probes ad0f638 / arceus-vk10-validation 85e72fd, Arm's own driver
    case Q_QUERY_LOD: return g->arm_rel != 0;
    // mali-all e6f6d0c (the Mali white-screen fix), mali_white_vk.inc mw_init: arm = g_arm_rel != 0; g_mw.want = mw_default_route (arm, DM_PORT_GALAR);
    //   en = mw_enabled (arm, DM_PORT_GALAR, le, se), 1 or 0 with the switches unset. DM_PORT_GALAR → P->mali_lit_standin (a game whose profile says it; part 1
    //   also checks e6f6d0c's own DM_PORT_GALAR for the real profiles)
    case Q_B10_STANDIN: return ref_mw_default_route(g->arm_rel != 0, P->mali_lit_standin) == 1;   // (MW_ROUTE_A)
    case Q_MALI_WHITE_CHECK: return ref_mw_enabled(g->arm_rel != 0, P->mali_lit_standin, NULL, NULL) == 1;
    }
    return -999;   // a quirk with no reference here: add its rule above
}
// 6e0e1dd's background-pipeline rule, before beta-exit f09e58f (6e0e1dd :3925 v = e ? atoi (e) != 0 : !DM_GAME_ENG_ASYNC_PSO_OFF && g_props.vendorID == 0x5143): part 1
//   checks that the table differs from it exactly where f09e58f meant it to, on every GPU but Adreno in a game without async_pso_off, and nowhere for Legends: Arceus
static int ref_6e0e1dd_async_pso(const gpu_id *g, const profile *P) { return !P->async_pso_off && g->vendor == 0x5143; }

// ---- the grid of GPU descriptions ----
// Arm's own driver as nvn_vk.c finds it (6e0e1dd :599-601 with the float-controls block: by driver ID or, without driver properties, by vendor; :629 without that
//   block: by vendor and not a driver package). nofc: the block did not run (Vulkan 1.1 without VK_KHR_shader_float_controls)
static void arm_fields(gpu_id *g, int nofc, int beta) {
    int arm = g->driver_id >= 0 ? g->driver_id == 9 : g->vendor == 0x13B5 && (!nofc || !g->custom);
    g->arm_rel = arm ? (int)(g->driver_version >> 22) : 0; g->arm_p = arm ? (int)(g->driver_version >> 12 & 0x3ff) : 0; g->arm_beta = arm && g->driver_id >= 0 ? beta : 0;
}
typedef struct { const uint32_t *ven; int nven; const int *did; int ndid; const uint32_t *api; int napi; const char *const *name; int nname; const long *ram; int nram;
                 const uint32_t *dv; int ndv; int arm_all; } grid_t;
typedef void (*visit_fn)(const gpu_id *g, void *u);
// every combination; where Arm's driver can be found (Arm's vendor ID or driver ID) with arm_all, every release r20p0-r55p2 besides the grid's driver versions
static long grid_run(const grid_t *G, int port, unsigned eng, visit_fn fn, void *u) {
    uint32_t armdv[36 * 3 + 16]; int narm = 0;
    for (int rel = 20; rel <= 55; rel++) for (int p = 0; p <= 2; p++) armdv[narm++] = (uint32_t)rel << 22 | (uint32_t)p << 12;
    for (int d = 0; d < G->ndv && narm < (int)(sizeof armdv / sizeof *armdv); d++) armdv[narm++] = G->dv[d];
    long n = 0;
    for (int a = 0; a < G->nven; a++) for (int b = 0; b < G->ndid; b++) {
        int armish = G->arm_all && (G->ven[a] == 0x13B5 || G->did[b] == 9);
        const uint32_t *dvs = armish ? armdv : G->dv; int ndv = armish ? narm : G->ndv;
        for (int d = 0; d < ndv; d++) for (int ap = 0; ap < G->napi; ap++) for (int custom = 0; custom <= 1; custom++) for (int nofc = 0; nofc <= 1; nofc++) for (int beta = 0; beta <= 1; beta++) {
            gpu_id g; memset(&g, 0, sizeof g); g.port = port; g.eng = eng; g.vendor = G->ven[a]; g.driver_id = G->did[b]; g.api = G->api[ap]; g.driver_version = dvs[d]; g.custom = custom;
            if (nofc && !(g.driver_id < 0 && g.vendor == 0x13B5 && custom)) continue;   // (the two routes without driver properties differ only for a Mali driver package)
            arm_fields(&g, nofc, beta);
            if (beta && !g.arm_beta) continue;
            for (int c = 0; c < G->nname; c++) for (int r = 0; r < G->nram; r++) for (int dr = 0; dr <= 1; dr++) {
                snprintf(g.name, sizeof g.name, "%s", G->name[c]); g.ram_kb = G->ram[r]; g.dynren = dr; fn(&g, u); n++; }
        }
    }
    return n;
}
static void fail_line(const gpu_id *g, int q, int table, int want, const char *what) {
    printf("FAIL: %s, quirk %d (%s): table %d, reference %d; port %d eng %#x api %u.%u vendor %#x driver id %d version %#x (Arm r%dp%d%s)%s \"%s\" %ld kB dynren %d\n", what, q,
           Q_INFO[q].what, table, want, g->port, g->eng, VK_API_VERSION_MAJOR(g->api), VK_API_VERSION_MINOR(g->api), g->vendor, g->driver_id, g->driver_version, g->arm_rel,
           g->arm_p, g->arm_beta ? " beta" : "", g->custom ? " package" : "", g->name, g->ram_kb, g->dynren);
}
typedef struct { const profile *P; const char *what; long apso_changed, mali_on; } cmp_ctx;
static void cmp_all(const gpu_id *g, void *u) {   // part 1: this port's profile, the reference rules with its DM_GAME_ENG_*
    cmp_ctx *c = u;
    for (int q = 0; q < Q_N; q++) { int t = quirk_default(q, g), w = ref(q, g, c->P); checks++; if (t != w && fails++ < 25) fail_line(g, q, t, w, c->what); }
    // background pipelines against 6e0e1dd's rule: changed exactly on the GPUs other than Adreno, in a game without async_pso_off (beta-exit f09e58f)
    int t = quirk_default(Q_ASYNC_PSO, g), o = ref_6e0e1dd_async_pso(g, c->P), meant = g->vendor != 0x5143 && !c->P->async_pso_off;
    checks++; if ((t != o) != meant && fails++ < 25) fail_line(g, Q_ASYNC_PSO, t, o, "background pipelines against 6e0e1dd's rule: changed where beta-exit f09e58f did not mean it, or kept where it did (6e0e1dd: the second value)");
    c->apso_changed += t != o;
    // the white-screen fix against e6f6d0c with its own test of the game, the port (DM_PORT_GALAR): for the real profiles, mali_lit_standin stands for it exactly
    int gal = g->port == PORT_GALAR, wa = ref_mw_default_route(g->arm_rel != 0, gal) == 1, wc = ref_mw_enabled(g->arm_rel != 0, gal, NULL, NULL) == 1;
    checks++; if (quirk_default(Q_B10_STANDIN, g) != wa && fails++ < 25) fail_line(g, Q_B10_STANDIN, quirk_default(Q_B10_STANDIN, g), wa, "route A against e6f6d0c's DM_PORT_GALAR");
    checks++; if (quirk_default(Q_MALI_WHITE_CHECK, g) != wc && fails++ < 25) fail_line(g, Q_MALI_WHITE_CHECK, quirk_default(Q_MALI_WHITE_CHECK, g), wc, "the white-screen check against e6f6d0c's DM_PORT_GALAR");
    c->mali_on += quirk_default(Q_B10_STANDIN, g) != 0;
}
static void cmp_eng(const gpu_id *g, void *u) {   // part 2: this port with these bits, the reference rules for a game whose profile says the quirks the bits stand for
    (void)u; profile P = { .ids = "", .app = "", .port = g->port, .qc11_no_eds = !!(g->eng & ENG_QC11_NO_EDS), .async_pso_off = !!(g->eng & ENG_ASYNC_PSO_OFF),
                           .nvn_async_never = !!(g->eng & ENG_NVN_ASYNC_NEVER), .driver_selftests = !!(g->eng & ENG_DRIVER_SELFTESTS),
                           .mali_lit_standin = !!(g->eng & ENG_MALI_LIT_STANDIN) };
    for (int q = 0; q < Q_N; q++) { int t = quirk_default(q, g), w = ref(q, g, &P); checks++; if (t != w && fails++ < 25) fail_line(g, q, t, w, "port and engine_quirks bits"); }
}
// part 5: what a rule can answer before gpu_id is complete. Before the device is made (nvn_vk.c mtl_init) gpu_id has no driver ID (-1) and no Arm release: every rule
//   that quirk_late does not name must answer the same there (nvn_vk_quirk_default answers -1 for the late ones until then), and every late one must read one of
//   them somewhere (the list is not wider than it needs). And the driver check's questions (probe selftest.c st_gpu, which knows no game profile and no dynamic
//   rendering) are answered from the vendor and the driver ID: Q_FLOAT_CONTROLS (float-controls), Q_CLEAR_BY_PASS and Q_NO_DYNREN (3d-layered: how the engine
//   clears inside the pass, and whether it records its passes with dynamic rendering) must read nothing else
static int g_late_reads[Q_N];
static void cmp_late(const gpu_id *g, void *u) {
    (void)u; gpu_id e = *g; e.driver_id = -1; e.arm_rel = e.arm_p = e.arm_beta = 0;
    gpu_id f; memset(&f, 0, sizeof f); f.vendor = g->vendor; f.driver_id = g->driver_id;
    for (int q = 0; q < Q_N; q++) { int t = quirk_default(q, g), b = quirk_default(q, &e); checks++;
        if (t != b) { g_late_reads[q] = 1; if (!quirk_late(q) && fails++ < 25) fail_line(g, q, t, b, "a rule quirk_late does not name reads the driver ID or Arm's release (before the device: the second value)"); } }
    static const int PROBE_Q[3] = { Q_FLOAT_CONTROLS, Q_CLEAR_BY_PASS, Q_NO_DYNREN };
    for (int i = 0; i < 3; i++) { int q = PROBE_Q[i]; checks++; if (quirk_default(q, g) != quirk_default(q, &f) && fails++ < 25)
        fail_line(g, q, quirk_default(q, g), quirk_default(q, &f), "a rule the driver check asks reads more than the vendor and the driver ID (the vendor and driver ID alone: the second value)"); }
}
static void cmp_both(const gpu_id *g, void *u) { cmp_all(g, u); cmp_late(g, NULL); }
// part 6: where a setting came from (vk_quirks.h vkq_env_src / vkq_env_player): unset, the player's (env.txt), or the driver check's (the keys android_main.c names in
//   DM_DRIVER_CHECK_KEYS), each key matched whole
static void env_sources(void) {
    const char *K = "DM_VK_FLOAT_CONTROLS";
    unsetenv("DM_DRIVER_CHECK_KEYS"); unsetenv(K);
    CHECK(vkq_env_src(K) == 0 && !vkq_env_player(K), "unset: no source, no player's value");
    setenv(K, "1", 1);
    CHECK(vkq_env_src(K) == 1 && vkq_env_player(K) && !strcmp(vkq_env_player(K), "1"), "set, no driver check keys: the player's (env.txt)");
    static const struct { const char *keys; int want; } C[] = {
        { "DM_VK_FLOAT_CONTROLS", 2 }, { "DM_VK_DMAHEAP,DM_VK_FLOAT_CONTROLS", 2 }, { "DM_VK_FLOAT_CONTROLS,DM_VK_ZCOPY", 2 }, { "DM_VK_DMAHEAP,DM_VK_FLOAT_CONTROLS,DM_VK_ZCOPY", 2 },
        { "", 1 }, { "DM_VK_DMAHEAP", 1 }, { "DM_VK_FLOAT_CONTROLS_X", 1 }, { "XDM_VK_FLOAT_CONTROLS", 1 }, { "DM_VK_FLOAT", 1 }, { ",", 1 }, { "DM_VK_DMAHEAP,,DM_VK_FLOAT_CONTROLS", 2 } };
    for (unsigned i = 0; i < sizeof C / sizeof *C; i++) { setenv("DM_DRIVER_CHECK_KEYS", C[i].keys, 1);
        CHECK(vkq_env_src(K) == C[i].want && !vkq_env_player(K) == (C[i].want == 2), "DM_DRIVER_CHECK_KEYS=\"%s\": source %d, want %d", C[i].keys, vkq_env_src(K), C[i].want); }
    setenv("DM_DRIVER_CHECK_KEYS", "DM_VK_FLOAT_CONTROLS", 1); unsetenv(K);
    CHECK(vkq_env_src(K) == 0 && !vkq_env_player(K), "named by the driver check but unset (env.txt cannot unset it): no source");
    CHECK(vkq_env_src(NULL) == 0 && vkq_env_src("") == 0, "no name: no source");
    unsetenv("DM_DRIVER_CHECK_KEYS");
    // quirk_note: the first decision of a rule is kept (two threads may decide one lazily: the game's and the replay's)
    int a = quirk_note(Q_PASS_DEFER, 1, 2), b = quirk_note(Q_PASS_DEFER, 0, 0);
    CHECK(a == 1 && b == 0 && g_qset[Q_PASS_DEFER] && g_qval[Q_PASS_DEFER] == 1 && g_qenv[Q_PASS_DEFER] == 2, "quirk_note keeps the first decision of a rule, and its source");
}

// ---- part 3: the profiles as game.tsv has them (rows "key<TAB>value", # comments; game.py takes port / engine_quirks from tables_from when a profile has none) ----
static int tsv_get(const char *dir, const char *id, const char *key, char *out, size_t n) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s/game.tsv", dir, id); FILE *f = fopen(p, "r"); if (!f) return -1;
    char ln[4096]; int found = 0;
    while (!found && fgets(ln, sizeof ln, f)) {
        char *s = ln; while (*s == ' ' || *s == '\t') s++; if (*s == '#' || !*s || *s == '\n' || *s == '\r') continue;
        char *tab = strchr(ln, '\t'); if (!tab) continue; *tab = 0;
        char *k = ln, *e = tab; while (*k == ' ') k++; while (e > k && e[-1] == ' ') *--e = 0;
        if (strcmp(k, key)) continue;
        char *v = tab + 1; v[strcspn(v, "\r\n")] = 0; while (*v == ' ') v++; snprintf(out, n, "%s", v); found = 1;
    }
    fclose(f); return found;
}
static int tsv_get_base(const char *dir, const char *id, const char *key, char *out, size_t n) {   // the profile's own, else its tables_from's
    int r = tsv_get(dir, id, key, out, n); if (r) return r;
    char base[64]; if (tsv_get(dir, id, "tables_from", base, sizeof base) == 1 && *base) return tsv_get(dir, base, key, out, n);
    *out = 0; return 0;
}
// the next space-separated word of *s into w (0: none left)
static int next_word(const char **s, char *w, size_t n) { while (**s == ' ') (*s)++; size_t l = strcspn(*s, " "); if (!l) return 0; snprintf(w, n, "%.*s", (int)l, *s); *s += l; return 1; }
static int has_word(const char *words, const char *w) { char x[64]; const char *s = words; while (next_word(&s, x, sizeof x)) if (!strcmp(x, w)) return 1; return 0; }
static void profiles_check(const char *dir) {
    static const char *const KNOWN[8] = { "qc11_no_eds", "gpu_time_auto_only", "frame_hold_off", "async_pso_off", "framegen_game_rate", "nvn_async_never", "driver_selftests",
                                          "mali_lit_standin" };
    for (int i = 0; i < NPROF; i++) {
        const profile *P = &PROFILES[i]; const char *ids = P->ids; char id[32];
        while (next_word(&ids, id, sizeof id)) {
            char port[64] = "", eq[512] = "", w[64];
            int r = tsv_get_base(dir, id, "port", port, sizeof port); tsv_get_base(dir, id, "engine_quirks", eq, sizeof eq);
            CHECK(r >= 0, "cannot read %s/%s/game.tsv (run from dm/gfx/tests, or give the games folder)", dir, id);
            if (r < 0) continue;
            int pn = !strcmp(port, "galar") ? PORT_GALAR : !strcmp(port, "arceus") ? PORT_ARCEUS : !strcmp(port, "paldea") ? PORT_PALDEA : -1;
            CHECK(pn == P->port, "%s: port \"%s\" in game.tsv, %d here", id, port, P->port);
            const int want[8] = { P->qc11_no_eds, P->gpu_time_auto_only, P->frame_hold_off, P->async_pso_off, P->framegen_game_rate, P->nvn_async_never, P->driver_selftests,
                                  P->mali_lit_standin };
            for (int k = 0; k < 8; k++) CHECK(has_word(eq, KNOWN[k]) == want[k], "%s: engine_quirks %s is %d in game.tsv (\"%s\"), %d here", id, KNOWN[k], has_word(eq, KNOWN[k]), eq, want[k]);
            for (const char *s = eq; next_word(&s, w, sizeof w); ) { int known = 0; for (int k = 0; k < 8; k++) known |= !strcmp(w, KNOWN[k]);
                CHECK(known, "%s: engine_quirks %s is new: give it a column here (and a gpu_id.eng bit if it decides a GPU default)", id, w); }
        }
        CHECK(!strcmp(gpu_port_app(P->port), P->app), "port %d is named %s in the log, the app is %s", P->port, gpu_port_app(P->port), P->app);
    }
}

// ---- part 4: phones from players' logs ----
static gpu_id phone(const profile *P, uint32_t api, uint32_t ven, int did, int rel, int p, int beta, int custom, const char *name, long ram, int dynren) {
    gpu_id g; memset(&g, 0, sizeof g); g.port = P->port; g.eng = eng_of(P); g.api = api; g.vendor = ven; g.driver_id = did; g.custom = custom; g.ram_kb = ram; g.dynren = dynren;
    snprintf(g.name, sizeof g.name, "%s", name); g.driver_version = (uint32_t)rel << 22 | (uint32_t)p << 12; arm_fields(&g, 0, beta); return g;
}
#define QD(q, g) quirk_default(q, &(g))
static void phones(const profile *P) {
    const char *gm = P->app; int arc = P->port == PORT_ARCEUS;
    const uint32_t V13 = 1u << 22 | 3u << 12 | 231, V11 = 1u << 22 | 1u << 12 | 128;
    gpu_id s25 = phone(P, V13, 0x5143, 8, 0, 0, 0, 0, "Adreno (TM) 830", 11800000, 0);
    CHECK(QD(Q_NO_DYNREN, s25) && QD(Q_GATHER_BIAS, s25) && QD(Q_PASS_DEFER, s25) && QD(Q_BARRIER_RELAXED, s25) && QD(Q_IMPORT_MARK_OFF, s25) && !QD(Q_NO_EDS, s25)
          && !QD(Q_FLOAT_CONTROLS, s25) && !QD(Q_PIPE_KEEP, s25) && !QD(Q_GPU_TIME_OFF, s25) && !QD(Q_QUERY_LOD, s25),
          "%s, S25+ (Adreno 830, Qualcomm 1.3): render passes, gathers nudged, passes kept open, barriers relaxed, per-draw state; no Mali or Xclipse workarounds", gm);
    CHECK(QD(Q_ASYNC_PSO, s25) == !arc && QD(Q_NVN_ASYNC, s25) == !arc && QD(Q_DRIVER_SELFTESTS, s25) == arc,
          "%s, S25+: pipelines in the background and Faster rendering's Auto on (Sword / Shield / Scarlet / Violet), off for Legends: Arceus (#34, #3); its self-tests", gm);
    gpu_id a710 = phone(P, V11, 0x5143, 8, 0, 0, 0, 0, "Adreno (TM) 710", 5600000, 1);
    CHECK(QD(Q_NO_DYNREN, a710) && !QD(Q_BARRIER_RELAXED, a710) && QD(Q_PIPE_KEEP, a710) == 800, "%s, Adreno 710 on Qualcomm's 1.1 driver (#34, #209): render passes, full barriers, pipelines capped (5.6 GB)", gm);
    CHECK(QD(Q_NO_EDS, a710) == arc && QD(Q_ASYNC_PSO, a710) == !arc,
          "%s, Adreno 710 on 1.1: Legends: Arceus builds the strides into each pipeline and no pipelines in the background (#34); Sword / Shield / Scarlet / Violet keep both", gm);
    gpu_id a650 = phone(P, V11, 0x5143, 8, 0, 0, 0, 0, "Adreno (TM) 650", 7600000, 1);
    CHECK(!QD(Q_BARRIER_RELAXED, a650) && QD(Q_NO_DYNREN, a650), "%s, Adreno 650 on Qualcomm's 1.1 driver (Galar #209): full barriers", gm);
    gpu_id tu = phone(P, V13, 0x5143, 18, 0, 0, 0, 1, "Turnip Adreno (TM) 650", 7600000, 1);
    CHECK(QD(Q_BARRIER_RELAXED, tu) && QD(Q_NO_DYNREN, tu) && QD(Q_NVN_ASYNC, tu) == !arc, "%s, Turnip on the Adreno 650: barriers relaxed (not Qualcomm's old driver), render passes", gm);
    gpu_id g615 = phone(P, 1u << 22 | 3u << 12, 0x13B5, 9, 44, 1, 0, 0, "Mali-G615 MC2", 7800000, 1);
    CHECK(QD(Q_G615_SAFE, g615) && QD(Q_CLAMP_ZERO, g615) && QD(Q_NAN_MINMAX, g615) == 2 && QD(Q_VIEW_QUARANTINE, g615) == 2048 && QD(Q_TEX_HALF_UNSAFE, g615)
          && QD(Q_PIPE_KEEP, g615) == 800 && QD(Q_EXACT_MATH, g615) && QD(Q_INIT_LOCALS, g615) && QD(Q_FLOAT_CONTROLS, g615) && QD(Q_DMAHEAP, g615) && QD(Q_GPU_TIME_OFF, g615)
          && QD(Q_TF_CHECK, g615) && QD(Q_QUERY_LOD, g615) && !QD(Q_NO_DYNREN, g615) && !QD(Q_BARRIER_RELAXED, g615) && !QD(Q_NVN_ASYNC, g615),
          "%s, Mali-G615 MC2 on r44p1 (#82 #110): the full Arm set (and the new LOD query rule), full barriers", gm);
    CHECK(QD(Q_TF_SELFTEST, g615) && QD(Q_DRIVER_SELFTESTS, g615) == arc, "%s, Mali-G615 MC2: the transform feedback self-test where the game runs its self-tests (Legends: Arceus)", gm);
    gpu_id g925 = phone(P, 1u << 22 | 3u << 12, 0x13B5, 9, 49, 1, 1, 0, "Mali-G925-Immortalis MC12", 11800000, 1);
    CHECK(QD(Q_CLAMP_ZERO, g925) && !QD(Q_G615_SAFE, g925) && !QD(Q_TEX_HALF_UNSAFE, g925), "%s, Mali-G925 beta r49p1 (#111): erratum workaround (beta), no G615 set", gm);
    gpu_id g925r = phone(P, 1u << 22 | 3u << 12, 0x13B5, 9, 49, 1, 0, 0, "Mali-G925-Immortalis MC12", 11800000, 1);
    CHECK(!QD(Q_CLAMP_ZERO, g925r), "%s, Mali r49p1 release: the erratum fixed", gm);
    gpu_id panvk = phone(P, 1u << 22 | 3u << 12, 0x13B5, 20, 25, 0, 0, 1, "Mali-G615 MC6 (Panfrost)", 7800000, 1);
    CHECK(!QD(Q_FLOAT_CONTROLS, panvk) && !QD(Q_NAN_MINMAX, panvk) && !QD(Q_G615_SAFE, panvk) && !QD(Q_QUERY_LOD, panvk) && !QD(Q_TF_CHECK, panvk) && QD(Q_ARM_FORMATS, panvk)
          && QD(Q_DMAHEAP, panvk), "%s, PanVK on a Mali-G615: none of Arm's driver workarounds, Mali's format lists and dma-buf pools kept", gm);
    gpu_id xcl = phone(P, 1u << 22 | 3u << 12, 0x144D, 21, 0, 0, 0, 0, "Samsung Xclipse 940", 7800000, 1);
    CHECK(QD(Q_HOST_IMPORT_OFF, xcl) && QD(Q_DMAHEAP, xcl) && QD(Q_FLOAT_CONTROLS, xcl) && QD(Q_CLEAR_BY_PASS, xcl) && !QD(Q_BARRIER_RELAXED, xcl) && !QD(Q_NO_DYNREN, xcl),
          "%s, Xclipse 940 (#39 #41): Samsung's set, full barriers (#31)", gm);
    gpu_id bxm = phone(P, 1u << 22 | 1u << 12, 0x1010, 7, 0, 0, 0, 0, "PowerVR B-Series BXM-8-256", 3800000, 0);
    CHECK(QD(Q_PCACHE_GAPS_OFF, bxm) && QD(Q_PIPE_KEEP, bxm) == 800 && QD(Q_BC_GPU_OFF, bxm) && QD(Q_BC_DECODE, bxm) && QD(Q_GPU_TIME_OFF, bxm) && !QD(Q_BARRIER_RELAXED, bxm),
          "%s, PowerVR BXM-8-256 on 4 GB: no cache for gap pipelines, pipelines capped, BC on the CPU and decoded, no GPU timestamps", gm);
    gpu_id g5 = phone(P, 1u << 22 | 3u << 12, 0x1010, 7, 0, 0, 0, 0, "PowerVR D-Series DXT-48-1536", 11800000, 1);
    CHECK(QD(Q_BC_GPU_OFF, g5) && QD(Q_BC_DECODE, g5), "%s, Tensor G5 (PowerVR, Arceus #32): BC converted on the CPU, decoded to RGBA", gm);
    CHECK(QD(Q_ASYNC_PSO, tu) == !arc && QD(Q_ASYNC_PSO, g615) == !arc && QD(Q_ASYNC_PSO, g925) == !arc && QD(Q_ASYNC_PSO, panvk) == !arc && QD(Q_ASYNC_PSO, xcl) == !arc
          && QD(Q_ASYNC_PSO, bxm) == !arc && QD(Q_ASYNC_PSO, g5) == !arc,
          "%s: pipelines the lists lacked built in the background on Turnip, Mali (Arm's driver and PanVK), Xclipse and PowerVR too (beta-exit f09e58f; Adreno only before), "
          "never for Legends: Arceus (async_pso_off, #34)", gm);
    int gal = P->port == PORT_GALAR;
    CHECK(QD(Q_B10_STANDIN, g615) == gal && QD(Q_MALI_WHITE_CHECK, g615) == gal && QD(Q_B10_STANDIN, g925) == gal && QD(Q_MALI_WHITE_CHECK, g925) == gal
          && QD(Q_B10_STANDIN, g925r) == gal && QD(Q_MALI_WHITE_CHECK, g925r) == gal,
          "%s, Mali-G615 MC2 r44p1 and G925 r49p1 on Arm's own driver (#110 #111): the lit target as RGBA16F and the white-screen check for Sword / Shield only", gm);
    CHECK(!QD(Q_B10_STANDIN, panvk) && !QD(Q_MALI_WHITE_CHECK, panvk) && !QD(Q_B10_STANDIN, s25) && !QD(Q_MALI_WHITE_CHECK, s25) && !QD(Q_B10_STANDIN, tu)
          && !QD(Q_B10_STANDIN, xcl) && !QD(Q_MALI_WHITE_CHECK, xcl) && !QD(Q_B10_STANDIN, bxm) && !QD(Q_MALI_WHITE_CHECK, g5),
          "%s: no white-screen fix on PanVK (Arm's vendor ID, its own driver ID), Adreno, Turnip, Xclipse or PowerVR", gm);
}

int main(int argc, char **argv) {
    const char *games = argc > 1 ? argv[1] : "../../games";
    for (int q = 0; q < Q_N; q++) { gpu_id z; memset(&z, 0, sizeof z);
        CHECK(Q_INFO[q].what && Q_INFO[q].env, "quirk %d has no Q_INFO row", q); CHECK(ref(q, &z, &PROFILES[0]) != -999, "quirk %d (%s) has no reference rule here", q, Q_INFO[q].what); }
    // 1. each port's profile against the reference (6e0e1dd's sites; Q_ASYNC_PSO beta-exit 3d21471's), over the whole grid
    static const uint32_t vens[] = { 0x5143, 0x13B5, 0x144D, 0x1010, 0x10DE, 0x10005, 0 };
    static const int dids[] = { -1, 9 /* Arm */, 21 /* Samsung */, 8 /* Qualcomm */, 18 /* Turnip */, 20 /* PanVK */, 7 /* Imagination */, 13 /* llvmpipe */, 0 };
    static const uint32_t apis[] = { 1u << 22, 1u << 22 | 1u << 12 | 128, 1u << 22 | 2u << 12 | 200, 1u << 22 | 3u << 12 | 231, 1u << 22 | 4u << 12 };
    static const char *const names[] = { "Mali-G615 MC2", "Mali-G720-Immortalis MC12", "Adreno (TM) 830", "Adreno (TM) 710", "Turnip Adreno (TM) 650", "Samsung Xclipse 940",
                                         "PowerVR D-Series DXT-48-1536", "llvmpipe (LLVM 15.0.7, 256 bits)", "" };
    static const long rams[] = { 0, 3800000, 7499999, 7500000, 11800000 };
    static const uint32_t dvs[] = { 0, 44u << 22 | 1u << 12, 48u << 22, 49u << 22, 0x80000000u | 512u << 12 | 762u, 0x5d9576 };   // (none, r44p1, r48p0, r49p0, Qualcomm 512.762, PowerVR)
    const grid_t G = { vens, 7, dids, 9, apis, 5, names, 9, rams, 5, dvs, 6, 1 };
    for (int i = 0; i < NPROF; i++) {
        long c0 = checks; cmp_ctx c = { &PROFILES[i], PROFILES[i].app, 0, 0 };
        long n = grid_run(&G, PROFILES[i].port, eng_of(&PROFILES[i]), cmp_both, &c);
        printf("%s (port %d, engine_quirks bits %#x): %ld GPU descriptions x %d quirks = %ld comparisons with the reference sites (and part 5's, before the device); "
               "background pipelines unlike 6e0e1dd on %ld of them (beta-exit f09e58f)\n", PROFILES[i].app, PROFILES[i].port, eng_of(&PROFILES[i]), n, Q_N, checks - c0, c.apso_changed);
        CHECK(PROFILES[i].async_pso_off ? c.apso_changed == 0 : c.apso_changed > 0, "%s: background pipelines unlike 6e0e1dd on %ld GPU descriptions (async_pso_off %d)", PROFILES[i].app,
              c.apso_changed, PROFILES[i].async_pso_off);
        printf("%s: the white-screen fix (route A and the check) by default on %ld of them\n", PROFILES[i].app, c.mali_on);
        CHECK(PROFILES[i].port == PORT_GALAR && MALI_LIT_DEFAULT ? c.mali_on > 0 : c.mali_on == 0, "%s: the white-screen fix by default on %ld GPU descriptions (Sword / Shield only)", PROFILES[i].app, c.mali_on);
    }
    // 5. (the rest) every late rule reads what comes after the device somewhere in the grid
    for (int q = 0; q < Q_N; q++) CHECK(!quirk_late(q) || g_late_reads[q], "quirk %d (%s) is in quirk_late but reads neither the driver ID nor Arm's release anywhere in the grid", q, Q_INFO[q].what);
    // 2. every port with every combination of the bits (a smaller grid: the bits act with the vendor, the version and the name)
    {   static const char *const nm2[] = { "Mali-G615 MC2", "Adreno (TM) 710", "Turnip Adreno (TM) 650", "" }; static const long ram2[] = { 0, 3800000 };
        static const uint32_t dv2[] = { 0, 38u << 22 | 1u << 12, 44u << 22 | 1u << 12, 49u << 22, 49u << 22 | 1u << 12, 52u << 22 };
        const grid_t G2 = { vens, 7, dids, 9, apis, 5, nm2, 4, ram2, 2, dv2, 6, 0 };
        long c0 = checks, n = 0; const unsigned all = ENG_QC11_NO_EDS | ENG_ASYNC_PSO_OFF | ENG_DRIVER_SELFTESTS | ENG_NVN_ASYNC_NEVER | ENG_MALI_LIT_STANDIN;
        for (int port = PORT_GALAR; port <= PORT_PALDEA; port++) for (unsigned e = 0; e <= all; e++) if (!(e & ~all)) n += grid_run(&G2, port, e, cmp_eng, NULL);
        printf("every port x every engine_quirks bit combination: %ld GPU descriptions x %d quirks = %ld comparisons (no rule reads the port)\n", n, Q_N, checks - c0); }
    // 3. the profiles
    profiles_check(games);
    // 4. players' phones, in each game
    for (int i = 0; i < NPROF; i++) phones(&PROFILES[i]);
    {   gpu_id z; memset(&z, 0, sizeof z); z.arm_rel = 44; z.arm_p = 1;
        printf("new rule, not in 6e0e1dd: %s (Arm's own driver r44p1: %d; every game)\n", Q_INFO[Q_QUERY_LOD].what, quirk_default(Q_QUERY_LOD, &z)); }
    // 6. where a setting came from
    env_sources();
    printf(fails ? "%ld of %ld FAILED\n" : "all %ld passed\n", fails ? fails : checks, checks);
    return fails != 0;
}
