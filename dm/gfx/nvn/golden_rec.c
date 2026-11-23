// Golden-frame recorder: one frame, as the NVN layer hands it to the replay, written into a .nxgold file (golden_fmt.h) that the PC player (dm/gfx/golden) replays
// headless and compares with an expected picture. Off unless DM_GOLDEN_REC is set (golden_rec.h); the hooks in hle_nvn.c / nvn_vk.c then cost one load and a branch.
//
// What a frame needs and where it comes from:
//  - the command records of each submission, followed through JUMP / CALL (walk below), and the NVN objects they name (textures, programs, syncs);
//  - the memory the GPU reads: what the records name (vertex buffers, deferred handles, counters, indirect arguments), plus what the replay layer looks up by address
//    (GOLDEN_MEM: uniform / storage blocks, index and copy ranges) and texture storage (the backend's texture lookup), in 4 KB pages, written again only when a page
//    changed since it was last written (the CPU fills uniform buffers between submissions, the GPU writes some pages the next submission reads);
//  - the registrations, the shaders by key, the replay state the frame starts from (as records, PROL), and the images whose content comes from earlier frames (TEXI).
// While recording, each submission waits for the one before (golden_be_finish): what is read then is what that submission reads on the GPU. The replay layer's
// mid-submission flushes (DM_VK_SPLIT) are turned off for the run.
#include "golden_rec.h"
#include "nvn_hmap.h"
#include "game_profile.h"
#include <zlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/uio.h>

#define GLOG(...) do { fprintf(stderr, "[dm] [golden] " __VA_ARGS__); fputc('\n', stderr); } while (0)

// The backend's side (golden_vk.inc): weak, so a build whose backend has none (the Metal one) still links and simply cannot record
#define W __attribute__((weak))
W int golden_be_pools(golden_be_pool_t *out, int max); W int golden_be_pool_of(uint64_t addr, uint64_t *lo, uint64_t *hi);
W int golden_be_treg_n(void); W int golden_be_treg_get(int id, tex_t *t, view_t *v, int *has_v); W int golden_be_sreg_n(void); W int golden_be_sreg_get(int id, samp_par *p);
W void golden_be_globals(int *origin_ll, int *depth_zero, int *tex_pool_n, char *gpu, size_t ngpu); W size_t golden_be_state_ops(uint8_t *buf, size_t cap);
W int golden_be_shader(const uint8_t key[8], int *stage, void **spv, size_t *nspv, void **vkr, size_t *nvkr, void **gl, size_t *ngl);
W void golden_be_frame_begin(void); W void golden_be_frame_end(void); W void golden_be_finish(void); W int golden_be_shot(const tex_t *t, const int crop[4], const char *path);
#undef W

int g_golden = -1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_npres;                       // presents seen
static long g_targets[64]; static int g_ntargets, g_ti;   // DM_GOLDEN_REC=<present>,...: the presents that end a recorded frame
static int g_req_mode; static long g_req_target = -1;
static int g_start_next;                       // the frame to record starts with the next submission
static long g_target;                          // the present that ends the frame being recorded
static char g_label[64], g_dir[1024], g_path[1200];
static FILE *g_f; static int g_fail;
static uint32_t g_sub_no;                      // submissions recorded in this frame (pages are checked once per submission)

// ---------- File ----------
static void put_chunk(uint32_t type, const void *data, size_t n, int compress) {
    if (!g_f || g_fail) return;
    golden_chunk_hdr h = { type, 0, n, n };
    const void *out = data; uint8_t *z = NULL;
    if (compress && n > 64) {
        uLongf zn = compressBound((uLong)n); z = malloc(zn);
        if (z && compress2(z, &zn, data, (uLong)n, 1) == Z_OK && zn < n) { h.flags = GCF_ZLIB; h.stored = zn; out = z; }
    }
    if (fwrite(&h, sizeof h, 1, g_f) != 1 || (h.stored && fwrite(out, 1, (size_t)h.stored, g_f) != h.stored)) { g_fail = 1; GLOG("[RED] writing %s failed (errno %d)", g_path, errno); }
    free(z);
}
typedef struct { uint8_t *p; size_t n, cap; } gbuf;
static void gb_put(gbuf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { size_t c = b->cap ? b->cap : 65536; while (c < b->n + n) c *= 2; uint8_t *q = realloc(b->p, c); if (!q) { g_fail = 1; return; } b->p = q; b->cap = c; }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static gbuf g_mem;                             // MEM runs waiting for the next submission
static gbuf g_live, g_cpuw;                    // LIVE / CPUW serials waiting
static gbuf g_subm_pending;                    // the submission being replayed (written once its memory is)
static uint64_t g_mem_bytes, g_mem_pages, g_texi_bytes;
static void flush_pending(void) {
    if (g_mem.n) { put_chunk(GC_MEM, g_mem.p, g_mem.n, 1); g_mem.n = 0; }
    if (g_live.n) { put_chunk(GC_LIVE, g_live.p, g_live.n, 1); g_live.n = 0; }
    if (g_cpuw.n) { put_chunk(GC_CPUW, g_cpuw.p, g_cpuw.n, 1); g_cpuw.n = 0; }
}

// ---------- Memory, page by page ----------
typedef struct { uint64_t page, hash; uint32_t sub; uint32_t used; } pent;
static pent *g_pages; static uint32_t g_pcap, g_pn;
static pent *page_slot(uint64_t page) {
    if ((g_pn + 1) * 4 > g_pcap * 3) {
        pent *o = g_pages; uint32_t oc = g_pcap; g_pcap = oc ? oc * 2 : 1u << 16; g_pages = calloc(g_pcap, sizeof *g_pages); g_pn = 0;
        if (!g_pages) { g_fail = 1; g_pages = o; g_pcap = oc; return NULL; }
        for (uint32_t i = 0; i < oc; i++) if (o[i].used) { pent *s = page_slot(o[i].page); *s = o[i]; g_pn++; }
        free(o);
    }
    for (uint32_t i = (uint32_t)((page >> 12) * 0x9e3779b97f4a7c15ull >> 32) & (g_pcap - 1);; i = (i + 1) & (g_pcap - 1)) {
        if (!g_pages[i].used) { g_pages[i].page = page; return &g_pages[i]; }
        if (g_pages[i].page == page) return &g_pages[i];
    }
}
// Memory read without trusting the address: a stale pointer in a command must not take the game down with the recording (process_vm_readv answers EFAULT)
static int safe_read(void *dst, uint64_t src, size_t n) {
    struct iovec l = { dst, n }, r = { (void *)(uintptr_t)src, n };
    ssize_t k = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (k == (ssize_t)n) return 1;
    if (k < 0 && (errno == ENOSYS || errno == EPERM)) { memcpy(dst, (const void *)(uintptr_t)src, n); return 1; }
    return 0;
}
static void mem_ref_locked(uint64_t addr, uint64_t len) {
    if (!len || !addr || g_fail) return;
    if (addr >> 47) { static int said; if (!said++) GLOG("[RED] address %#llx is outside the PC's 47-bit user space: not recorded (the player cannot map it)", (unsigned long long)addr); return; }
    uint64_t lo = 0, hi = 0, end = addr + len; int pooled = golden_be_pool_of(addr, &lo, &hi);
    if (pooled && end > hi) end = hi;
    if (!pooled && len > (64u << 20)) { len = 64u << 20; end = addr + len; }   // (outside any pool: an object or command memory, never this large)
    static uint8_t buf[4096];
    for (uint64_t pg = addr & ~4095ull; pg < end; pg += 4096) {
        uint64_t a = pg, b = pg + 4096;
        if (pooled) { if (a < lo) a = lo; if (b > hi) b = hi; }
        if (a >= b) continue;
        pent *s = page_slot(pg); if (!s) return;
        if (s->used && s->sub == g_sub_no + 1) continue;   // (checked in this submission already)
        int was = s->used; if (!was) g_pn++;
        if (!safe_read(buf, a, (size_t)(b - a))) { s->used = 1; s->sub = g_sub_no + 1; s->hash = 0; continue; }
        uint64_t h = nvn_hm_hash(buf, (uint32_t)(b - a)) | 1;
        int same = was && s->hash == h;
        s->used = 1; s->sub = g_sub_no + 1; s->hash = h;
        if (same) continue;
        golden_mem_run r = { a, b - a }; gb_put(&g_mem, &r, sizeof r); gb_put(&g_mem, buf, (size_t)(b - a));
        g_mem_bytes += b - a; g_mem_pages++;
        if (g_mem.n > (8u << 20)) { put_chunk(GC_MEM, g_mem.p, g_mem.n, 1); g_mem.n = 0; }
    }
}
void golden_mem_ref(uint64_t addr, uint64_t len) {
    pthread_mutex_lock(&g_mu); if (g_golden == 2) mem_ref_locked(addr, len); pthread_mutex_unlock(&g_mu);
}

// ---------- Shaders, programs, textures ----------
static nvn_hmap g_keys = {.klen = 8}, g_progs = {.klen = 8}, g_sers = {.klen = 8};
static void prog_ref(const prog_t *p) {
    if (!p) return;
    mem_ref_locked((uint64_t)(uintptr_t)p, sizeof *p);
    uint64_t ser; if (!safe_read(&ser, (uint64_t)(uintptr_t)&p->serial, 8) || !ser || nvn_hm_get(&g_progs, &ser)) return;
    nvn_hm_put(&g_progs, &ser, (void *)1);
    golden_prog g; memset(&g, 0, sizeof g); g.serial = ser; g.nstage = p->nstage > 6 ? 6 : p->nstage;
    for (unsigned i = 0; i < g.nstage; i++) {
        if (!prog_key(p, i, g.key[i])) continue;
        g.has |= 1u << i;
        if (nvn_hm_get(&g_keys, g.key[i])) continue;
        nvn_hm_put(&g_keys, g.key[i], (void *)1);
        int st = -1; void *spv = NULL, *vkr = NULL, *gl = NULL; size_t ns = 0, nr = 0, ngl = 0;
        if (!golden_be_shader(g.key[i], &st, &spv, &ns, &vkr, &nr, &gl, &ngl)) {
            GLOG("[RED] shader %02x%02x%02x%02x%02x%02x%02x%02x: not found (not translated on this phone?): the player will lack it", g.key[i][0], g.key[i][1], g.key[i][2], g.key[i][3], g.key[i][4], g.key[i][5], g.key[i][6], g.key[i][7]);
            continue; }
        golden_shdr h; memcpy(h.key, g.key[i], 8); h.stage = st; h.nspv = (uint32_t)ns; h.nvkr = (uint32_t)nr; h.ngl = (uint32_t)ngl;
        gbuf b = {0}; gb_put(&b, &h, sizeof h); gb_put(&b, spv, ns); gb_put(&b, vkr, nr); if (ngl) gb_put(&b, gl, ngl);
        put_chunk(GC_SHDR, b.p, b.n, 1); free(b.p); free(spv); free(vkr); free(gl);
    }
    put_chunk(GC_PROG, &g, sizeof g, 0);
}
static void tex_obj_ref(const tex_t *t) {   // a texture object a record names (its storage and image: the backend's lookup, golden_note_*)
    if (!t) return;
    mem_ref_locked((uint64_t)(uintptr_t)t, sizeof *t);
    const pool_t *pl; if (safe_read(&pl, (uint64_t)(uintptr_t)&t->pool, sizeof pl) && pl) mem_ref_locked((uint64_t)(uintptr_t)pl, sizeof(pool_t));
}
void golden_note_serial(uint64_t serial, int live, int cpu_written) {   // (backend, under its texture lookup; g_mu not held)
    pthread_mutex_lock(&g_mu);
    if (g_golden == 2 && serial && !nvn_hm_get(&g_sers, &serial)) {
        nvn_hm_put(&g_sers, &serial, (void *)1);
        if (live) gb_put(&g_live, &serial, 8);
        if (cpu_written) gb_put(&g_cpuw, &serial, 8);
    }
    pthread_mutex_unlock(&g_mu);
}
void golden_note_texi(const golden_texi *h, const uint8_t *data, size_t n) {
    pthread_mutex_lock(&g_mu);
    if (g_golden == 2) { gbuf b = {0}; gb_put(&b, h, sizeof *h); gb_put(&b, data, n); put_chunk(GC_TEXI, b.p, b.n, 1); free(b.p); g_texi_bytes += n; }
    pthread_mutex_unlock(&g_mu);
}

// ---------- Command records ----------
static void walk(const uint8_t *p, int depth) {
    if (!p || depth > 16) return;
    for (int guard = 0; guard < (1 << 24); guard++) {
        op_hdr h; if (!safe_read(&h, (uint64_t)(uintptr_t)p, sizeof h)) { GLOG("[RED] command record at %p not readable", (const void *)p); return; }
        uint32_t sz = h.size >= 8 ? h.size : 8;
        mem_ref_locked((uint64_t)(uintptr_t)p, sz);
        uint8_t a[sizeof(p_set_rt) + 64]; size_t pl = sz - 8 < sizeof a ? sz - 8 : sizeof a; memset(a, 0, sizeof a);
        if (pl && !safe_read(a, (uint64_t)(uintptr_t)p + 8, pl)) return;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); walk(t, depth + 1); break; }
            case OP_SIGNAL_EVENT: { uint64_t v; memcpy(&v, a, 8); mem_ref_locked(v, 4); break; }
            case OP_REPORT_COUNTER: { uint64_t ad; memcpy(&ad, a + 8, 8); mem_ref_locked(ad, 16); break; }
            case OP_FENCE_SYNC: { uint64_t s; memcpy(&s, a, 8); mem_ref_locked(s, sizeof(sync_t)); break; }
            case OP_RENDER_COND: { const p_render_cond *r = (const p_render_cond *)a; if (r->mode < 2) mem_ref_locked(r->addr, 32); break; }
            case OP_COPY_B2B: { uint64_t s, n; memcpy(&s, a, 8); memcpy(&n, a + 16, 8); mem_ref_locked(s, n); break; }
            case OP_SET_RT: { const p_set_rt *r = (const p_set_rt *)a; for (int i = 0; i < r->n && i < 8; i++) tex_obj_ref(r->color[i]); tex_obj_ref(r->depth); break; }
            case OP_BIND_PROGRAM: prog_ref(((const p_bind_program *)a)->prog); break;
            case OP_BIND_VBUF: { const p_bind_vbuf *b = (const p_bind_vbuf *)a; for (int i = 0; i < b->n && i < 16; i++) mem_ref_locked(b->r[i].address, b->r[i].size); break; }
            case OP_BIND_UBO: { const p_bind_buf *b = (const p_bind_buf *)a; mem_ref_locked(b->addr, 65536); break; }   // (the shader may read its whole 64 KB block)
            case OP_BIND_SSBO: { const p_bind_buf *b = (const p_bind_buf *)a; mem_ref_locked(b->addr, b->size ? b->size + 4096 : 65536); break; }
            case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: { const p_bind_handles *b = (const p_bind_handles *)a; if (b->defer && b->src && b->n > 0 && b->n <= 32) mem_ref_locked(b->src, 8 * (uint64_t)b->n); break; }
            case OP_COPY_B2T: case OP_COPY_T2B: tex_obj_ref(((const p_copy_b2t *)a)->tex); break;
            case OP_COPY_T2T: { const p_copy_t2t *c = (const p_copy_t2t *)a; tex_obj_ref(c->src); tex_obj_ref(c->dst); break; }
            case OP_CLEAR_TEX: tex_obj_ref(((const p_clear_tex *)a)->tex); break;
            case OP_TF_BIND: { const p_tf_bind *b = (const p_tf_bind *)a; mem_ref_locked(b->addr, b->size); break; }
            case OP_TF_STATE: mem_ref_locked(((const p_tf_state *)a)->ctl, 32); break;
            case OP_DRAW: { const p_draw *d = (const p_draw *)a; if (d->ind) mem_ref_locked(d->ind, 16); if (d->mdi_cnt) mem_ref_locked(d->mdi_cnt, 4); break; }
            case OP_DRAW_IDX: { const p_draw_idx *d = (const p_draw_idx *)a; uint64_t isz = d->itype == 0 ? 1 : d->itype == 1 ? 2 : 4, cnt = (uint64_t)(d->count > 0 ? d->count : 0);
                if (d->ind) { uint32_t args[5]; mem_ref_locked(d->ind, 20); if (d->mdi_cnt) mem_ref_locked(d->mdi_cnt, 4);
                    cnt = safe_read(args, d->ind, sizeof args) ? (uint64_t)args[0] + args[2] : 0; if (cnt * isz < 262144) cnt = 262144 / isz; }   // (the GPU may write the arguments: generous)
                mem_ref_locked(d->ibuf, cnt * isz); break; }
            case OP_DISPATCH: { const p_dispatch *d = (const p_dispatch *)a; if (d->ind) mem_ref_locked(d->ind, 12); break; }
            default: break;
        }
        p += sz;
    }
}

// ---------- Registrations and pools: the whole table at the frame's start, then what changed before each submission ----------
static golden_treg *g_treg; static int g_ntreg;
static golden_sreg *g_sreg; static int g_nsreg;
static golden_be_pool_t *g_pl; static int g_npl;
static void tables_sync(int full) {
    gbuf b = {0};
    int n = golden_be_treg_n();
    if (n > g_ntreg) { g_treg = realloc(g_treg, (size_t)n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (size_t)(n - g_ntreg) * sizeof *g_treg); for (int i = g_ntreg; i < n; i++) g_treg[i].id = -1; g_ntreg = n; }
    for (int i = 0; i < n; i++) {
        golden_treg e; memset(&e, 0, sizeof e); e.id = i; e.ok = golden_be_treg_get(i, &e.t, &e.v, &e.has_v);
        if (!e.ok) memset(&e.t, 0, sizeof e.t);
        if (!e.has_v) memset(&e.v, 0, sizeof e.v);
        if ((full && e.ok) || (!full && g_treg[i].id >= 0 && memcmp(&e, &g_treg[i], sizeof e)) || (!full && g_treg[i].id < 0 && e.ok)) gb_put(&b, &e, sizeof e);
        g_treg[i] = e;
    }
    if (b.n) put_chunk(GC_TREG, b.p, b.n, 1); b.n = 0;
    n = golden_be_sreg_n();
    if (n > g_nsreg) { g_sreg = realloc(g_sreg, (size_t)n * sizeof *g_sreg); memset(g_sreg + g_nsreg, 0, (size_t)(n - g_nsreg) * sizeof *g_sreg); for (int i = g_nsreg; i < n; i++) g_sreg[i].id = -1; g_nsreg = n; }
    for (int i = 0; i < n; i++) {
        golden_sreg e; memset(&e, 0, sizeof e); e.id = i; e.ok = golden_be_sreg_get(i, &e.p);
        if ((full && e.ok) || (!full && memcmp(&e, &g_sreg[i], sizeof e) && (e.ok || g_sreg[i].id >= 0))) gb_put(&b, &e, sizeof e);
        g_sreg[i] = e;
    }
    if (b.n) put_chunk(GC_SREG, b.p, b.n, 1); b.n = 0;
    int np = golden_be_pools(NULL, 0); golden_be_pool_t *pl = calloc(np ? (size_t)np : 1, sizeof *pl); np = golden_be_pools(pl, np);
    for (int i = 0; i < np; i++) {
        int known = 0; for (int k = 0; k < g_npl && !known; k++) known = g_pl[k].addr == pl[i].addr && g_pl[k].size == pl[i].size;
        if (full || !known) { golden_pool p = { pl[i].addr, pl[i].size }; gb_put(&b, &p, sizeof p); }
    }
    if (b.n) put_chunk(GC_POOL, b.p, b.n, 0);
    free(g_pl); g_pl = pl; g_npl = np; free(b.p);
}

// ---------- A frame ----------
static uint64_t g_rec_t0;
static void meta(void) {
    int oll = 0, dz = 0, tpn = 0; char gpu[256] = "";
    golden_be_globals(&oll, &dz, &tpn, gpu, sizeof gpu);
    const char *title = getenv("DM_PROGRAM_ID"); if (!title || !*title) title = DM_GAME_ID;
    time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm); char date[32]; strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", &tm);
    char m[2048]; int n = snprintf(m, sizeof m, "format=%d\ntitle=%s\nengine=%s\npresent=%ld\nlabel=%s\ngpu=%s\ndate=%s\ntex_t=%zu\nview_t=%zu\nsamp_par=%zu\nprog_t=%zu\norigin_ll=%d\ndepth_zero=%d\ntex_pool_n=%d\n",
                                   GOLDEN_VERSION, title, DM_GAME_ID, g_target, g_label, gpu, date, sizeof(tex_t), sizeof(view_t), sizeof(samp_par), sizeof(prog_t), oll, dz, tpn);
    put_chunk(GC_META, m, (size_t)n, 0);
}
static void frame_begin(void) {
    const char *title = getenv("DM_PROGRAM_ID"); if (!title || !*title) title = DM_GAME_ID;
    mkdir(g_dir, 0755);
    snprintf(g_path, sizeof g_path, "%s/%s-%s-p%ld.nxgold", g_dir, title, g_label[0] ? g_label : "frame", g_target);
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    g_f = fopen(tmp, "wb"); g_fail = !g_f;
    if (!g_f) { GLOG("[RED] cannot write %s (errno %d): recording skipped", tmp, errno); return; }
    golden_file_hdr fh; memcpy(fh.magic, GOLDEN_MAGIC, 8); fh.version = GOLDEN_VERSION; fh.hdr_size = sizeof fh; fwrite(&fh, sizeof fh, 1, g_f);
    golden_be_finish(); golden_be_frame_begin();
    g_golden = 2; g_sub_no = 0; g_mem_bytes = g_mem_pages = g_texi_bytes = 0; g_rec_t0 = (uint64_t)time(NULL);
    // (a fresh page / program / shader / texture record for every recording)
    free(g_pages); g_pages = NULL; g_pcap = g_pn = 0;
    free(g_keys.keys); free(g_keys.vals); nvn_hm_init(&g_keys, 8); free(g_progs.keys); free(g_progs.vals); nvn_hm_init(&g_progs, 8); free(g_sers.keys); free(g_sers.vals); nvn_hm_init(&g_sers, 8);
    free(g_treg); g_treg = NULL; g_ntreg = 0; free(g_sreg); g_sreg = NULL; g_nsreg = 0; free(g_pl); g_pl = NULL; g_npl = 0;
    meta();
    tables_sync(1);
    size_t cap = 1u << 20; uint8_t *ops = malloc(cap); size_t n = ops ? golden_be_state_ops(ops, cap) : 0;
    if (n) { walk(ops, 0); flush_pending(); put_chunk(GC_PROL, ops, n, 1); }
    else GLOG("[RED] the replay state could not be written: the frame starts from the default state");
    free(ops);
    GLOG("recording the frame up to present %ld into %s", g_target, g_path);
}
static void next_target(void) {
    g_target = -1;
    while (g_ti < g_ntargets && g_targets[g_ti] <= (long)g_npres) g_ti++;
    if (g_ti < g_ntargets) { g_target = g_targets[g_ti++]; g_golden = 1; }
    else g_golden = g_req_mode ? 1 : 0;
    g_start_next = g_target == (long)g_npres + 1;
}
static void frame_end(const tex_t *t, const int crop[4]) {
    if (t) tex_obj_ref(t);
    flush_pending();
    golden_pres p; memset(&p, 0, sizeof p); p.tex_addr = (uint64_t)(uintptr_t)t; if (crop) memcpy(p.crop, crop, sizeof p.crop); if (t) safe_read(&p.t, (uint64_t)(uintptr_t)t, sizeof p.t);
    put_chunk(GC_PRES, &p, sizeof p, 0);
    put_chunk(GC_END, NULL, 0, 0);
    int ok = g_f && !g_fail; if (g_f && fclose(g_f)) ok = 0; g_f = NULL;
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    if (ok && !rename(tmp, g_path)) GLOG("recorded %s: %u submissions, %llu pages (%.1f MB) of memory, %.1f MB of images", g_path, g_sub_no, (unsigned long long)g_mem_pages, g_mem_bytes / 1048576.0, g_texi_bytes / 1048576.0);
    else { unlink(tmp); GLOG("[RED] recording %s failed", g_path); ok = 0; }
    golden_be_frame_end();
    g_golden = 1;   // (the phone's own picture of the frame: not recorded)
    if (ok && t) { char png[1300]; snprintf(png, sizeof png, "%.*s.phone.png", (int)(strlen(g_path) - 7), g_path); golden_be_shot(t, crop, png); }
}

// ---------- Arming ----------
static void init_locked(void) {
    const char *e = getenv("DM_GOLDEN_REC");
    g_golden = 0;
    if (!e || !*e || !strcmp(e, "0")) return;
    if (!golden_be_pools || !golden_be_state_ops || !golden_be_shot) { GLOG("[RED] DM_GOLDEN_REC: this build's replay layer cannot record (no golden backend)"); return; }
    if (!strcmp(e, "req")) g_req_mode = 1;
    else for (const char *q = e; *q && g_ntargets < 64;) { char *x; long v = strtol(q, &x, 10); if (x == q) break; if (v > 0) g_targets[g_ntargets++] = v; q = *x == ',' ? x + 1 : x; }
    const char *lb = getenv("DM_GOLDEN_LABEL"); snprintf(g_label, sizeof g_label, "%s", lb && *lb ? lb : "frame");
    const char *d = getenv("DM_GOLDEN_DIR"), *pc = getenv("DM_VK_PCACHE");
    if (d && *d) snprintf(g_dir, sizeof g_dir, "%s", d);
    else if (pc && strrchr(pc, '/')) snprintf(g_dir, sizeof g_dir, "%.*s/golden", (int)(strrchr(pc, '/') - pc), pc);
    else snprintf(g_dir, sizeof g_dir, "golden");
    if (!getenv("DM_VK_SPLIT")) setenv("DM_VK_SPLIT", "0", 1);   // (whole submissions: each one's memory is read before any of it runs)
    next_target();
    GLOG("armed: %s, recordings into %s", g_req_mode ? "waiting for golden.req" : e, g_dir);
}
static void poll_request(void) {   // golden.req next to the pipeline cache: "<frames to wait> [label]"
    const char *pc = getenv("DM_VK_PCACHE"); if (!pc || !strrchr(pc, '/')) return;
    char req[1100]; snprintf(req, sizeof req, "%.*s/golden.req", (int)(strrchr(pc, '/') - pc), pc);
    FILE *f = fopen(req, "r"); if (!f) return;
    int wait = 0; char lb[64] = ""; int k = fscanf(f, "%d %63s", &wait, lb); fclose(f); unlink(req);
    if (k < 1 || wait < 0) wait = 0;
    if (k == 2) snprintf(g_label, sizeof g_label, "%s", lb);
    g_target = (long)g_npres + 1 + wait; g_start_next = wait == 0;
    GLOG("golden.req: recording the frame that ends with present %ld (label %s)", g_target, g_label);
}

void golden_sub_begin(int n, const uint64_t *h) {
    pthread_mutex_lock(&g_mu);
    if (g_golden < 0) init_locked();
    if (g_golden == 1 && g_start_next) { g_start_next = 0; frame_begin(); }
    if (g_golden == 2) {
        golden_be_finish();   // (the submission before is done: memory read now is what this one reads)
        tables_sync(0);
        for (int i = 0; i < n; i++) walk((const uint8_t *)(uintptr_t)h[i], 0);
        golden_subm sh = { (uint32_t)n, 0 }; g_subm_pending.n = 0; gb_put(&g_subm_pending, &sh, sizeof sh); gb_put(&g_subm_pending, h, 8 * (size_t)n);
    }
    pthread_mutex_unlock(&g_mu);
}
void golden_sub_end(void) {
    pthread_mutex_lock(&g_mu);
    if (g_golden == 2 && g_subm_pending.n) { flush_pending(); put_chunk(GC_SUBM, g_subm_pending.p, g_subm_pending.n, 0); g_subm_pending.n = 0; g_sub_no++; }
    pthread_mutex_unlock(&g_mu);
}
void golden_present(const tex_t *t, const int crop[4]) {
    pthread_mutex_lock(&g_mu);
    if (g_golden < 0) init_locked();
    g_npres++;
    if (g_golden == 1 && g_target == (long)g_npres && !g_f) { g_start_next = 0; frame_begin(); }   // (a frame without submissions)
    if (g_golden == 2 && (long)g_npres >= g_target) { frame_end(t, crop); next_target(); }
    else if (g_golden == 1) {
        if (g_req_mode && g_target < 0 && g_npres % 30 == 0) poll_request();
        if (g_target == (long)g_npres + 1) g_start_next = 1;
    }
    pthread_mutex_unlock(&g_mu);
}
