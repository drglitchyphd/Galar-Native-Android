// Golden-frame player: replays a recording (.nxgold, dm/gfx/nvn/golden_fmt.h) through the engine's own Vulkan replay layer (nvn_vk.c, built for the PC: dm/pc) on
// whatever Vulkan device the loader gives (Mesa lavapipe in WSL; VK_ICD_FILENAMES / VK_INSTANCE_LAYERS / VK_LOADER_* as for any Vulkan program, DM_VK_DEVICE=<index or
// name part> to pick one), headless, and writes the frame's picture as a PNG.
//   golden_play <recording.nxgold> <out.png> [--info] [--keep <shader folder>]
// Exit: 0 picture written, 1 the replay ran but made no picture, 2 the recording cannot be replayed here (format, memory, engine).
// How: the recording's memory goes back at the addresses the phone had (mapped before anything else is), the registrations, shaders and images from earlier frames are
// set up as the game had them, and each submission is replayed with mtl_exec, one after another, each waited for (the recorder read memory that way).
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include "nvn_mtl.h"
#include "nvn_dirty.h"
#include "golden_rec.h"
#include "dmsh_chain.h"
void dm_guest_write_prepare(void *addr, size_t len);   // (dm/host/dm.h; dm/pc/pc_host.c on the PC: the write tracking unprotects the pages first)
#include "game_profile.h"

#define PLOG(...) do { fprintf(stderr, "[golden_play] " __VA_ARGS__); fputc('\n', stderr); } while (0)
static int g_quiet;

// ---------- The file ----------
typedef struct { golden_chunk_hdr h; const uint8_t *data; } chunk;
static const uint8_t *g_file; static size_t g_fsize;
static chunk *g_ch; static int g_nch;
static void *inflate_chunk(const chunk *c) {   // malloc'd payload (raw bytes)
    void *o = malloc(c->h.raw ? c->h.raw : 1); if (!o) return NULL;
    if (!(c->h.flags & GCF_ZLIB)) { memcpy(o, c->data, c->h.raw); return o; }
    uLongf n = (uLongf)c->h.raw;
    if (uncompress(o, &n, c->data, (uLong)c->h.stored) != Z_OK || n != c->h.raw) { free(o); return NULL; }
    return o;
}
static int file_open(const char *path) {
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { PLOG("cannot open %s (errno %d)", path, errno); return 0; }
    g_fsize = (size_t)st.st_size; g_file = mmap(NULL, g_fsize, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (g_file == MAP_FAILED || g_fsize < sizeof(golden_file_hdr)) { PLOG("%s: not readable", path); return 0; }
    golden_file_hdr fh; memcpy(&fh, g_file, sizeof fh);
    if (memcmp(fh.magic, GOLDEN_MAGIC, 8) || fh.version != GOLDEN_VERSION) { PLOG("%s: not a golden recording of version %d", path, GOLDEN_VERSION); return 0; }
    size_t off = fh.hdr_size; int cap = 0;
    while (off + sizeof(golden_chunk_hdr) <= g_fsize) {
        chunk c; memcpy(&c.h, g_file + off, sizeof c.h); off += sizeof c.h;
        if (c.h.stored > g_fsize - off) { PLOG("%s: truncated (chunk %d)", path, g_nch); return 0; }
        c.data = g_file + off; off += c.h.stored;
        if (g_nch == cap) { cap = cap ? cap * 2 : 256; g_ch = realloc(g_ch, (size_t)cap * sizeof *g_ch); }
        g_ch[g_nch++] = c;
        if (c.h.type == GC_END) break;
    }
    if (!g_nch || g_ch[g_nch - 1].h.type != GC_END) { PLOG("%s: no END chunk (recording cut short)", path); return 0; }
    return 1;
}

// ---------- Memory at the phone's addresses ----------
typedef struct { uint64_t lo, hi; } range;
static range *g_rg; static int g_nrg, g_crg;
static void rg_add(uint64_t lo, uint64_t hi) { if (g_nrg == g_crg) { g_crg = g_crg ? g_crg * 2 : 1024; g_rg = realloc(g_rg, (size_t)g_crg * sizeof *g_rg); } g_rg[g_nrg++] = (range){ lo & ~4095ull, (hi + 4095) & ~4095ull }; }
static int rg_cmp(const void *a, const void *b) { const range *x = a, *y = b; return x->lo < y->lo ? -1 : x->lo > y->lo; }
static int map_all(void) {
    qsort(g_rg, (size_t)g_nrg, sizeof *g_rg, rg_cmp);
    int n = 0; for (int i = 0; i < g_nrg; i++) { if (n && g_rg[i].lo <= g_rg[n - 1].hi) { if (g_rg[i].hi > g_rg[n - 1].hi) g_rg[n - 1].hi = g_rg[i].hi; } else g_rg[n++] = g_rg[i]; }
    g_nrg = n; uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        uint64_t lo = g_rg[i].lo, len = g_rg[i].hi - lo;
        if (g_rg[i].hi > 0x7ffffffff000ull) { PLOG("memory at %#llx is beyond this PC's user address space: cannot replay", (unsigned long long)lo); return 0; }
        void *p = mmap((void *)(uintptr_t)lo, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != (void *)(uintptr_t)lo) { PLOG("memory at %#llx+%#llx cannot be mapped here (errno %d: %s)", (unsigned long long)lo, (unsigned long long)len, errno, errno == EEXIST ? "this PC's process has something there" : "refused"); return 0; }
        total += len;
    }
    if (!g_quiet) PLOG("%d memory ranges mapped at the phone's addresses (%.1f MB of address space)", n, total / 1048576.0);
    return 1;
}

// ---------- Shaders and programs ----------
typedef struct { uint64_t serial; golden_prog p; } progent;
static golden_prog *g_progs; static int g_nprogs;
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) {
    for (int i = g_nprogs - 1; i >= 0; i--) if (g_progs[i].serial == p->serial) {
        if (stage >= g_progs[i].nstage || !(g_progs[i].has >> stage & 1)) return 0;
        memcpy(key, g_progs[i].key[stage], 8); return 1; }
    return 0;
}
static const char *stage_name(int s) { static const char *n[] = { "vertex", "pixel", "geometry", "hull", "domain", "compute" }; return s >= 0 && s < 6 ? n[s] : "vertex"; }
static int write_file(const char *path, const void *d, size_t n) { FILE *f = fopen(path, "wb"); if (!f) return 0; int ok = fwrite(d, 1, n, f) == n; return fclose(f) == 0 && ok; }
static int shaders_out(const char *dir) {   // every shader into <dir>/b-<chain>/<key>.spv / .vkr / .gl.spv, the map into <dir>/shader_map.tsv: the replay's file route
    char p[1400]; snprintf(p, sizeof p, "%s/b-%s", dir, DMSH_CHAIN); mkdir(dir, 0755); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/shader_map.tsv", dir); FILE *map = fopen(p, "w"); if (!map) return 0;
    fprintf(map, "package\tindex\tstage\tkey\tsource\n"); int n = 0;
    for (int i = 0; i < g_nch; i++) if (g_ch[i].h.type == GC_SHDR) {
        uint8_t *d = inflate_chunk(&g_ch[i]); if (!d || g_ch[i].h.raw < sizeof(golden_shdr)) { free(d); continue; }
        golden_shdr h; memcpy(&h, d, sizeof h); const uint8_t *b = d + sizeof h;
        if (sizeof h + (uint64_t)h.nspv + h.nvkr + h.ngl > g_ch[i].h.raw) { free(d); continue; }
        char hex[17]; for (int k = 0; k < 8; k++) sprintf(hex + 2 * k, "%02x", h.key[k]);
        snprintf(p, sizeof p, "%s/b-%s/%s.spv", dir, DMSH_CHAIN, hex); write_file(p, b, h.nspv);
        snprintf(p, sizeof p, "%s/b-%s/%s.vkr", dir, DMSH_CHAIN, hex); write_file(p, b + h.nspv, h.nvkr);
        if (h.ngl) { snprintf(p, sizeof p, "%s/b-%s/%s.gl.spv", dir, DMSH_CHAIN, hex); write_file(p, b + h.nspv + h.nvkr, h.ngl); }
        fprintf(map, "golden\t%d\t%s\t%s\trecording\n", n++, stage_name(h.stage), hex); free(d);
    }
    fclose(map);
    snprintf(p, sizeof p, "%s/shader_map.tsv", dir); setenv("DM_NVN_SHMAP", p, 1); setenv("DM_SPV_DIR", dir, 1); unsetenv("DM_SPV_PAK"); unsetenv("DM_LIVE_SPV");
    return n;
}

// ---------- META ----------
static char g_meta[4096];
static const char *meta(const char *key) {
    static char vs[32][512]; static int k; char *v = vs[k++ & 31]; size_t kl = strlen(key);   // (several in one printf)
    for (const char *l = g_meta; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, key, kl) && l[kl] == '=') { const char *e = strchr(l, '\n'); size_t n = e ? (size_t)(e - l - kl - 1) : strlen(l + kl + 1); if (n >= sizeof vs[0]) n = sizeof vs[0] - 1; memcpy(v, l + kl + 1, n); v[n] = 0; return v; }
    return NULL;
}
static int in_family(const char *title) {   // this engine build runs the games of its family (Pokémon Shield's runs Sword as well)
    static const struct { const char *id, *stem; } fam[] = DM_GAME_FAMILY;
    for (unsigned i = 0; i < sizeof fam / sizeof *fam; i++) if (!strcasecmp(fam[i].id, title)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL, *keep = NULL; int info = 0;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--info")) info = 1; else if (!strcmp(argv[i], "--quiet")) g_quiet = 1; else if (!strcmp(argv[i], "--keep") && i + 1 < argc) keep = argv[++i]; else if (!in) in = argv[i]; else if (!out) out = argv[i]; }
    if (!in || (!out && !info)) { fprintf(stderr, "usage: golden_play <recording.nxgold> <out.png> [--info] [--quiet] [--keep <shader folder>]\n"); return 2; }
    if (!file_open(in)) return 2;
    // what the file holds, and the memory it needs, before anything else takes address space
    size_t nsub = 0, nmem = 0, nshdr = 0, ntexi = 0;
    for (int i = 0; i < g_nch; i++) {
        chunk *c = &g_ch[i];
        if (c->h.type == GC_META && !g_meta[0]) { void *d = inflate_chunk(c); if (d) { size_t n = c->h.raw < sizeof g_meta - 1 ? c->h.raw : sizeof g_meta - 1; memcpy(g_meta, d, n); g_meta[n] = 0; free(d); } }
        else if (c->h.type == GC_POOL) { golden_pool *p = inflate_chunk(c); for (size_t k = 0; p && k < c->h.raw / sizeof *p; k++) rg_add(p[k].addr, p[k].addr + p[k].size); free(p); }
        else if (c->h.type == GC_MEM) { uint8_t *d = inflate_chunk(c); size_t o = 0;
            while (d && o + sizeof(golden_mem_run) <= c->h.raw) { golden_mem_run r; memcpy(&r, d + o, sizeof r); o += sizeof r + r.len; rg_add(r.addr, r.addr + r.len); nmem += r.len; }
            free(d); }
        else if (c->h.type == GC_SUBM) nsub++; else if (c->h.type == GC_SHDR) nshdr++; else if (c->h.type == GC_TEXI) ntexi++;
    }
    const char *title = meta("title"), *engine = meta("engine");
    if (info || !g_quiet) PLOG("%s: title %s, engine %s, present %s, label %s, recorded on %s, %s; %zu submissions, %zu shaders, %zu images, %.1f MB of memory",
        in, title ? title : "?", engine ? engine : "?", meta("present") ? meta("present") : "?", meta("label") ? meta("label") : "?", meta("gpu") ? meta("gpu") : "?", meta("date") ? meta("date") : "?", nsub, nshdr, ntexi, nmem / 1048576.0);
    if (info) return 0;
    if (!title || !in_family(title)) { PLOG("title %s is not a game of this engine build (%s): build the player with GAME=<its engine>", title ? title : "?", DM_GAME_ID); return 2; }
    const char *ts = meta("tex_t"), *vs = meta("view_t"), *ss = meta("samp_par");
    if (!ts || atoi(ts) != (int)sizeof(tex_t) || !vs || atoi(vs) != (int)sizeof(view_t) || !ss || atoi(ss) != (int)sizeof(samp_par)) { PLOG("the recording's NVN objects differ from this build's (tex_t %s here %zu): rebuild both from one commit", ts ? ts : "?", sizeof(tex_t)); return 2; }
    if (!map_all()) return 2;
    char dir[1100]; if (keep) snprintf(dir, sizeof dir, "%s", keep); else { snprintf(dir, sizeof dir, "/tmp/golden_play.XXXXXX"); if (!mkdtemp(dir)) { PLOG("no temporary folder"); return 2; } }
    int nsh = shaders_out(dir);
    if (!getenv("DM_VK_SPLIT")) setenv("DM_VK_SPLIT", "0", 1);
    unsetenv("DM_VK_PCACHE"); unsetenv("DM_GOLDEN_REC");
    { const char *o = meta("origin_ll"), *z = meta("depth_zero"), *n = meta("tex_pool_n");
      if (o) mtl_set_origin_lower_left(atoi(o)); if (z) mtl_set_depth_zero(atoi(z)); if (n && atoi(n) > 0) mtl_set_tex_pool_n(atoi(n)); }
    mtl_init();
    // GOLDEN_SKIP=texi,prol,mem,treg,live (diagnosis, and the round-trip test's check that its picture depends on them): those chunks left out
    const char *skip = getenv("GOLDEN_SKIP"); if (skip && !g_quiet) PLOG("GOLDEN_SKIP=%s: those chunks are left out", skip);
    int rc = 1, nsubs = 0;
    for (int i = 0; i < g_nch; i++) {
        chunk *c = &g_ch[i]; uint8_t *d = NULL;
        if (skip) { char t[5] = { 0 }; memcpy(t, &c->h.type, 4); for (int k = 0; k < 4; k++) t[k] = t[k] == ' ' ? 0 : (char)(t[k] | 0x20); if (*t && strcasestr(skip, t)) continue; }
        switch (c->h.type) {
            case GC_POOL: { golden_pool *p = inflate_chunk(c); for (size_t k = 0; p && k < c->h.raw / sizeof *p; k++) { mtl_pool_add((const void *)(uintptr_t)p[k].addr, p[k].size); nvn_dirty_add_range((const void *)(uintptr_t)p[k].addr, p[k].size); } free(p); break; }
            case GC_MEM: { d = inflate_chunk(c); size_t o = 0;
                while (d && o + sizeof(golden_mem_run) <= c->h.raw) { golden_mem_run r; memcpy(&r, d + o, sizeof r); o += sizeof r;
                    if (o + r.len > c->h.raw) break;
                    dm_guest_write_prepare((void *)(uintptr_t)r.addr, (size_t)r.len); memcpy((void *)(uintptr_t)r.addr, d + o, (size_t)r.len); o += r.len; }
                break; }
            case GC_TREG: { golden_treg *t = inflate_chunk(c); for (size_t k = 0; t && k < c->h.raw / sizeof *t; k++) mtl_reg_tex(t[k].id, t[k].ok ? &t[k].t : NULL, t[k].has_v ? &t[k].v : NULL); free(t); break; }
            case GC_SREG: { golden_sreg *s = inflate_chunk(c); for (size_t k = 0; s && k < c->h.raw / sizeof *s; k++) if (s[k].ok) mtl_reg_samp(s[k].id, &s[k].p); free(s); break; }
            case GC_PROG: { golden_prog *p = inflate_chunk(c); size_t n = p ? c->h.raw / sizeof *p : 0; g_progs = realloc(g_progs, (g_nprogs + n) * sizeof *g_progs); memcpy(g_progs + g_nprogs, p, n * sizeof *p); g_nprogs += (int)n; free(p); break; }
            case GC_LIVE: case GC_CPUW: { uint64_t *s = inflate_chunk(c);
                for (size_t k = 0; s && k < c->h.raw / 8; k++) { tex_t t; memset(&t, 0, sizeof t); t.serial = s[k]; if (c->h.type == GC_LIVE) mtl_tex_init(&t); else mtl_tex_cpu_written(&t); }
                free(s); break; }
            case GC_TEXI: { d = inflate_chunk(c); if (d && c->h.raw >= sizeof(golden_texi)) { golden_texi h; memcpy(&h, d, sizeof h); golden_be_tex_fill(&h, d + sizeof h, c->h.raw - sizeof h); } break; }
            case GC_PROL: { d = inflate_chunk(c); if (d) { mtl_sub_begin(); mtl_exec(d); mtl_sub_commit(); mtl_finish(); } break; }
            case GC_SUBM: { d = inflate_chunk(c); if (!d) break; golden_subm s; memcpy(&s, d, sizeof s); const uint64_t *h = (const uint64_t *)(d + sizeof s);
                if (sizeof s + 8 * (uint64_t)s.n > c->h.raw) break;
                mtl_sub_begin(); mtl_sub_index(h, (int)s.n);
                for (uint32_t k = 0; k < s.n; k++) { mtl_exec_rest(h + k + 1, (int)(s.n - k - 1)); mtl_exec((const uint8_t *)(uintptr_t)h[k]); }
                mtl_exec_rest(NULL, 0); mtl_sub_commit(); mtl_finish(); nsubs++; break; }
            case GC_PRES: { golden_pres p; if (c->h.raw < sizeof p) break; d = inflate_chunk(c); if (!d) break; memcpy(&p, d, sizeof p);
                const tex_t *t = p.tex_addr ? (const tex_t *)(uintptr_t)p.tex_addr : &p.t;
                if (t->magic != p.t.magic || t->serial != p.t.serial) t = &p.t;   // (the object changed after the present: its copy)
                mtl_finish();
                if (golden_be_shot(t, p.crop, out)) { rc = 0; if (!g_quiet) PLOG("%d submissions replayed, %d shaders; picture %s", nsubs, nsh, out); }
                else PLOG("the presented texture has no image: no picture");
                break; }
            default: break;
        }
        free(d);
    }
    if (!keep) { char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {} }
    fflush(stderr);
    _exit(rc);   // (the replay's threads and driver are not torn down: nothing to keep)
}
