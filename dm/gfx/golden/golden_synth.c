// A golden frame made on the PC: this program plays the game's part (what hle_nvn.c does for a real game) through the PC build of the replay layer (dm/pc) with the
// recorder on, so the whole path is checked without a phone: NVN-level commands → replay layer (live picture) → recording (.nxgold) → golden_play in a fresh process →
// PNG → golden_check against the live picture.
//   golden_synth <out folder> [title ID]      (DM_GOLDEN_* set here; the title defaults to Pokémon Shield, 0100ABF008968000 makes it Sword's)
// Two frames, the second recorded, so that what a real frame depends on is exercised:
//  - frame 1 draws a vertex-coloured triangle into an offscreen target A (the recording must carry A's content: TEXI) and leaves the lit program, its vertex layout,
//    uniform blocks, texture handle and depth state bound (the recording must start from that state: PROL);
//  - frame 2, submission 1: a buffer → texture copy (checker C), clears, a textured quad sampling A at the back; the CPU then rewrites the vertex uniform block in place
//    (the recording must carry both contents, one per submission); submission 2: an inline uniform update (light colour), a lit, textured, depth-tested cube sampling C.
#define _GNU_SOURCE 1
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nvn_mtl.h"
#include "nvn_dirty.h"
#include "golden_rec.h"
#include "dmsh_chain.h"
#include "game_profile.h"

#define SLOG(...) do { fprintf(stderr, "[golden_synth] " __VA_ARGS__); fputc('\n', stderr); } while (0)
enum { WW = 320, WH = 240, AW = 128, CW = 64 };
#define POOL_ADDR 0x3000000000ull
#define POOL_SIZE (32u << 20)
#define HEAP_ADDR 0x3100000000ull
static uint8_t *g_pool_mem; static pool_t *g_pool; static uint8_t *g_heap; static size_t g_heap_used;
static void *heap_alloc_(size_t n) { void *p = g_heap + g_heap_used; g_heap_used += (n + 63) & ~(size_t)63; memset(p, 0, n); return p; }

// ---------- Shaders: keys, map, reflection (the replay's file route) ----------
typedef struct { const char *file, *stage, *vkr; uint8_t key[8]; } shader_def;
static shader_def g_sh[4] = {
    { "flat.vert", "vertex", "# flat.vert: no resources\n", { 'G', 'L', 'D', 'F', 'L', 'T', 'V', 1 } },
    { "flat.frag", "pixel", "# flat.frag: no resources\n", { 'G', 'L', 'D', 'F', 'L', 'T', 'F', 1 } },
    { "lit.vert", "vertex", "b vp_c1 cbuf 1 0 0 0 1\n", { 'G', 'L', 'D', 'L', 'I', 'T', 'V', 1 } },
    { "lit.frag", "pixel", "b fp_c1 cbuf 1 0 0 0 1\nb fp_t0 tex 0 0 0 0 1\n", { 'G', 'L', 'D', 'L', 'I', 'T', 'F', 1 } },
};
static int shaders_setup(const char *spv_in, const char *dir) {
    char p[1400], hex[17]; snprintf(p, sizeof p, "%s/b-%s", dir, DMSH_CHAIN); mkdir(dir, 0755); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/shader_map.tsv", dir); FILE *map = fopen(p, "w"); if (!map) return 0;
    fprintf(map, "package\tindex\tstage\tkey\tsource\n");
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 8; k++) sprintf(hex + 2 * k, "%02x", g_sh[i].key[k]);
        char src[1400]; snprintf(src, sizeof src, "%s/%s.spv", spv_in, g_sh[i].file);
        FILE *f = fopen(src, "rb"); if (!f) { SLOG("no %s (built by dm/pc/Makefile)", src); fclose(map); return 0; }
        static uint8_t buf[1 << 20]; size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
        snprintf(p, sizeof p, "%s/b-%s/%s.spv", dir, DMSH_CHAIN, hex); f = fopen(p, "wb"); if (!f) return 0; fwrite(buf, 1, n, f); fclose(f);
        snprintf(p, sizeof p, "%s/b-%s/%s.vkr", dir, DMSH_CHAIN, hex); f = fopen(p, "wb"); if (!f) return 0; fputs(g_sh[i].vkr, f); fclose(f);
        fprintf(map, "synth\t%d\t%s\t%s\tgolden_synth/%s\n", i, g_sh[i].stage, hex, g_sh[i].file);
    }
    fclose(map);
    snprintf(p, sizeof p, "%s/shader_map.tsv", dir); setenv("DM_NVN_SHMAP", p, 1); setenv("DM_SPV_DIR", dir, 1); unsetenv("DM_SPV_PAK");
    return 1;
}
// The game's side of prog_key (hle_nvn.c hashes the shader code; here each program names its two shaders)
static struct { uint64_t serial; int vs, fs; } g_progk[4]; static int g_nprogk;
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) {
    for (int i = 0; i < g_nprogk; i++) if (g_progk[i].serial == p->serial) { if (stage > 1) return 0; memcpy(key, g_sh[stage ? g_progk[i].fs : g_progk[i].vs].key, 8); return 1; }
    return 0;
}
static prog_t *mk_prog(int vs, int fs) {
    prog_t *p = heap_alloc_(sizeof *p); p->magic = MAGIC('P', 'R', 'O', 'G'); p->nstage = 2; p->serial = nvn_new_serial();
    g_progk[g_nprogk].serial = p->serial; g_progk[g_nprogk].vs = vs; g_progk[g_nprogk].fs = fs; g_nprogk++; return p;
}
// ---------- Textures, as nvnTextureInitialize makes them ----------
static uint64_t g_tex_off = 0x400000;
static tex_t *mk_tex(int fmt, uint32_t w, uint32_t h) {
    tex_t *t = heap_alloc_(sizeof *t);
    t->magic = MAGIC('T', 'E', 'X', 0); t->d.target = NVN_TEXTURE_TARGET_2D; t->d.format = fmt; t->d.width = w; t->d.height = h; t->d.depth = 1; t->d.levels = 1;
    t->d.flags = NVN_TEXTURE_FLAGS_COMPRESSIBLE;
    nvn_tex_layout(&t->d, &t->L);
    g_tex_off = (g_tex_off + 0xffff) & ~0xffffull; t->pool = g_pool; t->off = (int64_t)g_tex_off; t->mem = g_pool_mem + g_tex_off; g_tex_off += t->L.size;
    t->serial = nvn_new_serial(); mtl_tex_init(t);
    return t;
}
// ---------- Command records (nvn_obj.h), into the pool's command memory ----------
static uint8_t *g_cmd = NULL, *g_cmd_start;
static void cmd(uint32_t op, const void *a, size_t n) {
    uint32_t sz = (uint32_t)((8 + n + 7) & ~(size_t)7); op_hdr h = { op, sz };
    memcpy(g_cmd, &h, 8); memset(g_cmd + 8, 0, sz - 8); if (n) memcpy(g_cmd + 8, a, n); g_cmd += sz;
}
static void cmd_begin(void) { g_cmd_start = g_cmd; cmd(OP_BEGIN, NULL, 0); }
static uint64_t cmd_end(void) { cmd(OP_END, NULL, 0); uint64_t h = (uint64_t)(uintptr_t)g_cmd_start; g_cmd = (uint8_t *)(((uintptr_t)g_cmd + 255) & ~(uintptr_t)255); return h; }
static void submit(uint64_t h) {   // hle_nvn.c sub_run
    GOLDEN_SUB_BEGIN(1, &h);
    mtl_sub_begin(); mtl_sub_index(&h, 1); mtl_exec_rest(NULL, 0); mtl_exec((const uint8_t *)(uintptr_t)h); mtl_exec_rest(NULL, 0); mtl_sub_commit();
    GOLDEN_SUB_END();
}
static void present(const tex_t *t) { int crop[4] = { 0, 0, WW, WH }; GOLDEN_PRESENT(t, crop); mtl_present(t, 0, crop); }
static void set_rt(const tex_t *c, const tex_t *d) { p_set_rt r; memset(&r, 0, sizeof r); r.n = 1; r.color[0] = c; r.depth = d; cmd(OP_SET_RT, &r, sizeof r); }
static void viewport(int w, int h) {
    p_viewports v; memset(&v, 0, sizeof v); v.first = 0; v.n = 1; v.v[0][2] = (float)w; v.v[0][3] = (float)h; v.v[0][5] = 1; cmd(OP_VIEWPORTS, &v, sizeof v);
    p_scissor s = { 0, 0, w, h }; cmd(OP_SCISSOR, &s, sizeof s);
    p_depth_range r = { 0, 1 }; cmd(OP_DEPTH_RANGE, &r, sizeof r);
}
static void clear(float r, float g, float b) { p_clear_color c = { 0, 0xf, { r, g, b, 1 } }; cmd(OP_CLEAR_COLOR, &c, sizeof c); }
static void vbuf(uint64_t addr, uint64_t size) { p_bind_vbuf b; memset(&b, 0, sizeof b); b.first = 0; b.n = 1; b.r[0].address = addr; b.r[0].size = size; cmd(OP_BIND_VBUF, &b, sizeof b); }
static void vlayout(int n, const int *fmt, const int *off, uint32_t stride) {
    p_bind_vattr a; memset(&a, 0, sizeof a); a.first = 0; a.n = n;
    for (int i = 0; i < n; i++) { va_st s = { (uint32_t)fmt[i], (uint32_t)off[i], 0 }; memcpy(&a.st[i], &s, 4); }
    cmd(OP_BIND_VATTR, &a, sizeof a);
    p_bind_vstream v; memset(&v, 0, sizeof v); v.first = 0; v.n = 1; v.st[0].stride = stride; cmd(OP_BIND_VSTREAM, &v, sizeof v);
}
static void ubo(int stage, int index, uint64_t addr, uint64_t size) { p_bind_buf b = { stage, index, addr, size }; cmd(OP_BIND_UBO, &b, sizeof b); }
static void tex_handle(int stage, int slot, int tid, int sid) { p_bind_handles b; memset(&b, 0, sizeof b); b.stage = stage; b.first = slot; b.n = 1; b.h[0] = (uint64_t)tid | (uint64_t)sid << 20; cmd(OP_BIND_TEX, &b, sizeof b); }
static void program(const prog_t *p) { p_bind_program b = { p, 3 }; cmd(OP_BIND_PROGRAM, &b, sizeof b); }
static void depth_state(int test) { p_bind_state b = { 5 }; ds_st d = { (uint64_t)test, (uint64_t)test, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1 }; memcpy(b.bytes, &d, sizeof d); cmd(OP_BIND_STATE, &b, sizeof b); }
static void draw_idx(int count, uint64_t ibuf) { p_draw_idx d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_TRIANGLES; d.itype = NVN_INDEX_TYPE_UNSIGNED_SHORT; d.count = count; d.instances = 1; d.ibuf = ibuf; cmd(OP_DRAW_IDX, &d, sizeof d); }

// ---------- Matrices (column-major, as std140 mat4; clip z in [0, 1]: depth mode near = 0) ----------
typedef struct { float m[16]; } mat4;
static mat4 mul(mat4 a, mat4 b) { mat4 r; for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) { float s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k]; r.m[c * 4 + rr] = s; } return r; }
static mat4 ident(void) { mat4 r = {{0}}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
static mat4 rot(float ax, float ay) { mat4 x = ident(), y = ident(); float c = cosf(ax), s = sinf(ax); x.m[5] = c; x.m[6] = s; x.m[9] = -s; x.m[10] = c; c = cosf(ay); s = sinf(ay); y.m[0] = c; y.m[2] = -s; y.m[8] = s; y.m[10] = c; return mul(y, x); }
static mat4 persp(float fovy, float aspect, float n, float f) { mat4 r = {{0}}; float t = 1.0f / tanf(fovy / 2); r.m[0] = t / aspect; r.m[5] = t; r.m[10] = f / (n - f); r.m[11] = -1; r.m[14] = n * f / (n - f); return r; }
static mat4 trans(float x, float y, float z) { mat4 r = ident(); r.m[12] = x; r.m[13] = y; r.m[14] = z; return r; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: golden_synth <out folder> [title ID]\n"); return 2; }
    const char *outdir = argv[1], *title = argc > 2 ? argv[2] : DM_GAME_ID;
    char exe[1024], spv_in[1100], shdir[1100]; ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1); if (el <= 0) return 2; exe[el] = 0; *strrchr(exe, '/') = 0;
    snprintf(spv_in, sizeof spv_in, "%s/synth", exe); snprintf(shdir, sizeof shdir, "%s/synth-shaders", outdir); mkdir(outdir, 0755);
    setenv("DM_GOLDEN_REC", "2", 1); setenv("DM_GOLDEN_DIR", outdir, 1); setenv("DM_GOLDEN_LABEL", "synth", 1); setenv("DM_PROGRAM_ID", title, 1);
    if (!shaders_setup(spv_in, shdir)) return 2;
    // the game's memory: one pool (command memory, vertices, indices, uniform blocks, texture storage) and its object heap, at fixed addresses as a phone's would be
    g_pool_mem = mmap((void *)(uintptr_t)POOL_ADDR, POOL_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    g_heap = mmap((void *)(uintptr_t)HEAP_ADDR, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (g_pool_mem != (void *)(uintptr_t)POOL_ADDR || g_heap != (void *)(uintptr_t)HEAP_ADDR) { SLOG("cannot map the test memory (errno %d)", errno); return 2; }
    g_pool = heap_alloc_(sizeof *g_pool); g_pool->magic = MAGIC('P', 'O', 'O', 'L'); g_pool->flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED; g_pool->mem = g_pool_mem; g_pool->size = POOL_SIZE;
    mtl_set_depth_zero(1); mtl_set_origin_lower_left(0);
    mtl_pool_add(g_pool_mem, POOL_SIZE); nvn_dirty_add_range(g_pool_mem, POOL_SIZE);
    g_cmd = g_pool_mem;
    const uint64_t P = (uint64_t)(uintptr_t)g_pool_mem, VB = P + 0x100000, IB = P + 0x180000, UBV = P + 0x1C0000, UBF = P + 0x1C1000, CHK = P + 0x200000;
    // geometry: a triangle (position, colour), a quad and a cube (position, normal, uv), indices
    float tri[] = { 0, -0.8f, 0, 1, 0.2f, 0.1f,  0.8f, 0.8f, 0, 0.1f, 1, 0.2f,  -0.8f, 0.8f, 0, 0.2f, 0.3f, 1 };
    memcpy((void *)(uintptr_t)VB, tri, sizeof tri);
    float quad[] = { -1, -1, 0.95f, 0, 0, 1, 0, 0,  1, -1, 0.95f, 0, 0, 1, 1, 0,  1, 1, 0.95f, 0, 0, 1, 1, 1,  -1, 1, 0.95f, 0, 0, 1, 0, 1 };
    memcpy((void *)(uintptr_t)(VB + 0x1000), quad, sizeof quad);
    uint16_t qi[] = { 0, 1, 2, 0, 2, 3 }; memcpy((void *)(uintptr_t)IB, qi, sizeof qi);
    float cube[24 * 8]; uint16_t ci[36]; int nv = 0, ni = 0;
    static const float N[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
    for (int f = 0; f < 6; f++) {
        const float *n = N[f]; float u[3] = { n[1] != 0 ? 1.0f : 0.0f, 0, n[1] != 0 ? 0.0f : 1.0f }, v[3];
        if (n[0] != 0) { u[0] = 0; u[1] = 0; u[2] = 1; }
        v[0] = n[1] * u[2] - n[2] * u[1]; v[1] = n[2] * u[0] - n[0] * u[2]; v[2] = n[0] * u[1] - n[1] * u[0];
        for (int k = 0; k < 4; k++) { float a = (k == 1 || k == 2) ? 1.0f : -1.0f, b = k >= 2 ? 1.0f : -1.0f; float *o = &cube[nv * 8];
            for (int c = 0; c < 3; c++) { o[c] = 0.5f * (n[c] + a * u[c] + b * v[c]); o[3 + c] = n[c]; } o[6] = (a + 1) * 1.5f; o[7] = (b + 1) * 1.5f; nv++; }
        uint16_t b0 = (uint16_t)(f * 4); uint16_t q[6] = { b0, (uint16_t)(b0 + 1), (uint16_t)(b0 + 2), b0, (uint16_t)(b0 + 2), (uint16_t)(b0 + 3) }; memcpy(&ci[ni], q, sizeof q); ni += 6;
    }
    memcpy((void *)(uintptr_t)(VB + 0x2000), cube, sizeof cube); memcpy((void *)(uintptr_t)(IB + 0x100), ci, sizeof ci);
    // uniform blocks: vertex (mvp, model, tint), fragment (light direction, colour, ambient)
    struct { mat4 mvp, model; float tint[4]; } vu = { ident(), ident(), { 1, 1, 1, 1 } };
    memcpy((void *)(uintptr_t)UBV, &vu, sizeof vu);
    float fu[12] = { -0.4f, -0.5f, -1.0f, 0,  1.0f, 0.95f, 0.8f, 1,  0.25f, 0.25f, 0.3f, 1 }; memcpy((void *)(uintptr_t)UBF, fu, sizeof fu);
    // the checkerboard the copy reads (RGBA8, rows tight)
    for (int y = 0; y < CW; y++) for (int x = 0; x < CW; x++) { uint8_t *p = (uint8_t *)(uintptr_t)(CHK + 4 * (y * CW + x)); int on = ((x >> 3) ^ (y >> 3)) & 1;
        p[0] = on ? 240 : 30; p[1] = on ? 200 : 60; p[2] = on ? 40 : 150; p[3] = 255; }
    // objects: textures (A offscreen target, C checker, W window, D depth), samplers, programs
    tex_t *A = mk_tex(NVN_FORMAT_RGBA8, AW, AW), *C = mk_tex(NVN_FORMAT_RGBA8, CW, CW), *Wt = mk_tex(NVN_FORMAT_RGBA8, WW, WH), *D = mk_tex(NVN_FORMAT_DEPTH32F, WW, WH);
    mtl_reg_tex(1, A, NULL); mtl_reg_tex(2, C, NULL); mtl_set_tex_pool_n(64);
    samp_par lin = { NVN_MIN_FILTER_LINEAR, NVN_MAG_FILTER_LINEAR, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, 0, 0, 0, 0, 1000, 0, 1, { 0, 0, 0, 0 } };
    samp_par pt = { NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST, NVN_WRAP_MODE_REPEAT, NVN_WRAP_MODE_REPEAT, NVN_WRAP_MODE_REPEAT, 0, 0, 0, 0, 1000, 0, 1, { 0, 0, 0, 0 } };
    mtl_reg_samp(1, &lin); mtl_reg_samp(2, &pt);
    prog_t *flat = mk_prog(0, 1), *lit = mk_prog(2, 3);
    static const int fl_fmt[2] = { NVN_FORMAT_RGB32F, NVN_FORMAT_RGB32F }, fl_off[2] = { 0, 12 }, lt_fmt[3] = { NVN_FORMAT_RGB32F, NVN_FORMAT_RGB32F, NVN_FORMAT_RG32F }, lt_off[3] = { 0, 12, 24 };

    // ---- frame 1: the triangle into A; the lit state left bound for frame 2 ----
    cmd_begin();
    set_rt(A, NULL); viewport(AW, AW); clear(0.05f, 0.05f, 0.1f); depth_state(0);
    program(flat); vlayout(2, fl_fmt, fl_off, 24); vbuf(VB, sizeof tri);
    { p_draw d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_TRIANGLES; d.count = 3; d.instances = 1; cmd(OP_DRAW, &d, sizeof d); }
    program(lit); vlayout(3, lt_fmt, lt_off, 32); ubo(0, 1, UBV, 256); ubo(1, 1, UBF, 256); tex_handle(1, 0, 1, 1); depth_state(1);
    set_rt(Wt, D); viewport(WW, WH); clear(0, 0, 0);
    submit(cmd_end());
    present(Wt);
    // ---- frame 2 (recorded), submission 1: the checker copied in, clears, the quad sampling A at the back ----
    cmd_begin();
    { p_copy_b2t c; memset(&c, 0, sizeof c); c.src = CHK; c.tex = C; c.r.width = CW; c.r.height = CW; c.r.depth = 1; cmd(OP_COPY_B2T, &c, sizeof c); }
    set_rt(Wt, D); viewport(WW, WH); clear(0.2f, 0.25f, 0.3f);
    { p_clear_ds c = { 1.0f, 1, 0, 0xff }; cmd(OP_CLEAR_DS, &c, sizeof c); }
    vbuf(VB + 0x1000, sizeof quad); draw_idx(6, IB);
    submit(cmd_end());
    // the CPU rewrites the vertex block in place for the cube (the submission before is done with it)
    mtl_finish();
    mat4 model = rot(0.6f, 0.8f); vu.model = model; vu.mvp = mul(persp(1.0f, (float)WW / WH, 0.5f, 20.0f), mul(trans(0, 0, -3.0f), model)); vu.tint[0] = 1; vu.tint[1] = 0.9f; vu.tint[2] = 0.85f;
    memcpy((void *)(uintptr_t)UBV, &vu, sizeof vu);
    // submission 2: the light colour updated inline, the lit cube sampling C
    cmd_begin();
    { struct { uint64_t dst, n; float rgba[4]; } u = { UBF + 16, 16, { 1.0f, 0.85f, 0.7f, 1 } }; cmd(OP_UPDATE_UB, &u, sizeof u); }
    tex_handle(1, 0, 2, 2); vbuf(VB + 0x2000, sizeof cube); draw_idx(36, IB + 0x100);
    submit(cmd_end());
    present(Wt);
    mtl_finish();
    char rec[1400]; snprintf(rec, sizeof rec, "%s/%s-synth-p2.nxgold", outdir, title);
    struct stat st; if (stat(rec, &st)) { SLOG("no recording at %s", rec); _exit(1); }
    SLOG("recorded %s (%lld bytes); the live picture is %s-synth-p2.phone.png", rec, (long long)st.st_size, title);
    fflush(stderr); _exit(0);
}
