// Pipelines that crashed the driver (nvn_vk.c pso_make, prewarm_compute, dispatch): a crash-safe "pipeline in progress" marker and the skip list it feeds.
//   A Galaxy Z Fold4 (Adreno 730, Qualcomm's own 0615.87.2 driver, Vulkan 1.1.128) closed in libllvm-qgl.so CreateQGLCShader, called from vkCreateGraphicsPipelines
//   for one of Violet's first pipelines on the prewarm thread, at every start: a crash loop the safe-start ladder never left (its levels change textures and
//   settings, not which pipelines are built, and a driver change starts it over). Now each build first writes which shaders it is building into a fixed slot of a
//   small file next to the pipeline cache (pso_inflight: PG_SLOTS slots of 64 bytes, one per building thread, pwrite only: no malloc, nothing to flush) and
//   clears the slot after. The crash handler (host/crash.c → nvn_vk_pso_crash_note) marks the slot of the thread that crashed. The next start reads the file:
//   a marked slot's shaders go on the skip list at once; a slot left in progress but not marked (an abort the handler does not see, a kill) is a suspect, and a
//   suspect left in progress again by the next run goes on the list too (a player who closes the app while a pipeline builds loses nothing).
//   The list (pso_skip.txt, text, deletable) belongs to one driver: its first line names the driver (pg_ident: vendor, driver ID, driver version, name, driver
//   info), and a list of another driver is started over. A pipeline on the list is not built: the prewarm skips it, a draw that needs it is skipped like one
//   whose shader is missing, logged once in red. DM_VK_PSO_SKIP=0: no markers, no list.
//   Pure C on files and memory: dm/gfx/tests/t_pso_guard.c checks it on the PC.
#ifndef PSO_GUARD_H
#define PSO_GUARD_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PG_UNUSED __attribute__((unused))
enum { PG_SLOTS = 64, PG_MAX = 256, PG_MAGIC = 0x4f535047u /* "GPSO" */ };
enum { PG_GRAPHICS = 1, PG_COMPUTE = 2 };
// one slot of pso_inflight (64 bytes): what a thread is building now; magic 0 when it is not building
typedef struct { uint32_t magic, kind; uint64_t ident; int32_t tid; uint32_t crashed; uint8_t k0[8], k1[8]; uint8_t pad[24]; } pg_slot;
typedef char pg_slot_size_check[sizeof(pg_slot) == 64 ? 1 : -1];
// an entry of the list: the shader keys (vertex and fragment, or the compute shader and zeros) and its kind; said: logged this run
typedef struct { uint8_t k[16]; uint8_t kind, said; } pg_ent;
typedef struct { uint64_t ident; int n, ns; pg_ent e[PG_MAX]; pg_ent s[PG_MAX]; } pg_list;   // e: skipped; s: suspects (left in progress once, not marked)
// the driver as one number (FNV-1a 64 over what the driver says of itself)
PG_UNUSED static uint64_t pg_ident(uint32_t vendor, int driver_id, uint32_t driver_version, const char *name, const char *info) {
    char b[600]; int n = snprintf(b, sizeof b, "%#x/%d/%#x/%s/%s", vendor, driver_id, driver_version, name ? name : "", info ? info : "");
    uint64_t h = 1469598103934665603ull; for (int i = 0; i < n && i < (int)sizeof b; i++) { h ^= (uint8_t)b[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}
PG_UNUSED static int pg_find(const pg_ent *e, int n, int kind, const uint8_t k[16]) { for (int i = 0; i < n; i++) if (e[i].kind == kind && !memcmp(e[i].k, k, 16)) return i; return -1; }
PG_UNUSED static int pg_add(pg_ent *e, int *n, int kind, const uint8_t k[16]) {   // 1 added, 0 there already or full
    if (pg_find(e, *n, kind, k) >= 0 || *n >= PG_MAX) return 0;
    memset(&e[*n], 0, sizeof e[*n]); memcpy(e[*n].k, k, 16); e[*n].kind = (uint8_t)kind; (*n)++; return 1;
}
PG_UNUSED static void pg_keys(const uint8_t k0[8], const uint8_t k1[8], uint8_t k[16]) { memcpy(k, k0, 8); if (k1) memcpy(k + 8, k1, 8); else memset(k + 8, 0, 8); }
PG_UNUSED static void pg_hex(const uint8_t *k, int n, char *out) { for (int i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", k[i]); out[2 * n] = 0; }
PG_UNUSED static int pg_unhex(const char *s, uint8_t *k, int n) { for (int i = 0; i < n; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1; k[i] = (uint8_t)v; } return 0; }
// the list's file: "driver <ident>", then "skip|suspect graphics|compute <key0> <key1>" lines. Returns 1 read, 0 none (no file), -1 another driver's (started over)
PG_UNUSED static int pg_load(const char *path, uint64_t ident, pg_list *L) {
    memset(L, 0, sizeof *L); L->ident = ident;
    FILE *f = path ? fopen(path, "r") : NULL; if (!f) return 0;
    char ln[256]; unsigned long long id = 0; int ok = fgets(ln, sizeof ln, f) && sscanf(ln, "driver %llx", &id) == 1;
    if (!ok || (uint64_t)id != ident) { fclose(f); return -1; }
    while (fgets(ln, sizeof ln, f)) {
        char what[16], kind[16], a[40], b[40]; uint8_t k[16];
        if (sscanf(ln, "%15s %15s %39s %39s", what, kind, a, b) != 4 || strlen(a) != 16 || strlen(b) != 16 || pg_unhex(a, k, 8) || pg_unhex(b, k + 8, 8)) continue;
        int kd = !strcmp(kind, "compute") ? PG_COMPUTE : PG_GRAPHICS;
        if (!strcmp(what, "skip")) pg_add(L->e, &L->n, kd, k); else if (!strcmp(what, "suspect")) pg_add(L->s, &L->ns, kd, k);
    }
    fclose(f); return 1;
}
PG_UNUSED static int pg_save(const char *path, const pg_list *L) {
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path); FILE *f = fopen(tmp, "w"); if (!f) return 0;
    fprintf(f, "driver %016llx\n", (unsigned long long)L->ident);
    for (int w = 0; w < 2; w++) { const pg_ent *e = w ? L->s : L->e; int n = w ? L->ns : L->n;
        for (int i = 0; i < n; i++) { char a[17], b[17]; pg_hex(e[i].k, 8, a); pg_hex(e[i].k + 8, 8, b);
            fprintf(f, "%s %s %s %s\n", w ? "suspect" : "skip", e[i].kind == PG_COMPUTE ? "compute" : "graphics", a, b); } }
    int ok = !fclose(f); if (ok) ok = rename(tmp, path) == 0; return ok;
}
// what the last run left in pso_inflight, taken into the list: a marked slot (the thread crashed in the build) is skipped from now on; an unmarked one that was a
//   suspect already is skipped too, otherwise it becomes a suspect. Suspects the last run did not leave again are forgotten (a crash repeats; a closed app does not).
//   Another driver's slots count for nothing. Returns how many were added to the skip list (*suspects: how many suspects now)
PG_UNUSED static int pg_take(pg_list *L, const pg_slot *sl, int nsl, int *suspects) {
    pg_ent ns[PG_MAX]; int nns = 0, added = 0;
    for (int i = 0; i < nsl; i++) { const pg_slot *s = &sl[i];
        if (s->magic != PG_MAGIC || s->ident != L->ident || (s->kind != PG_GRAPHICS && s->kind != PG_COMPUTE)) continue;
        uint8_t k[16]; pg_keys(s->k0, s->k1, k);
        if (s->crashed || pg_find(L->s, L->ns, (int)s->kind, k) >= 0) added += pg_add(L->e, &L->n, (int)s->kind, k);
        else if (pg_find(L->e, L->n, (int)s->kind, k) < 0) pg_add(ns, &nns, (int)s->kind, k);
    }
    memcpy(L->s, ns, sizeof ns[0] * (size_t)nns); L->ns = nns;
    if (suspects) *suspects = nns;
    return added;
}
#endif
