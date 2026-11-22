// A golden frame made on the PC: this program plays the game's part (what hle_nvn.c does for a real game) through the PC build of the replay layer (dm/pc) with the
// recorder on, so the whole path is checked without a phone: NVN-level commands → replay layer (live picture) → recording (.nxgold) → golden_play in a fresh process →
// PNG → golden_check against the live picture.
//   golden_synth <out folder> [title ID]      (DM_GOLDEN_* set here; the title defaults to Pokémon Shield, 0100ABF008968000 makes it Sword's)
// Two frames, the second recorded, so that what a real frame depends on is exercised:
//  - frame 1 draws a vertex-coloured triangle into an offscreen target A (the recording must carry A's content: TEXI) and leaves the lit program, its vertex layout,
//    uniform blocks, texture handle and depth state bound (the recording must start from that state: PROL);
//  - frame 2, submission 1: a buffer → texture copy (checker C), clears, a textured quad sampling A at the back; the CPU then rewrites the vertex uniform block in place
//    (the recording must carry both contents, one per submission); submission 2: an inline uniform update (light colour), a lit, textured, depth-tested cube sampling C.
// GOLDEN_SYNTH_B10=1 adds the R11G11B10F paths the Mali float-target tests change (DM_VK_B10_AS_RGBA16 / DM_VK_B10_NOCOMPRESS; golden_check.py b10), the frame
//   otherwise the same: frame 1 draws into R11G11B10F targets (a lit pass L1: a clear with a negative and a large channel, a triangle writing alpha 0.25, then a white
//   one blended by the target's alpha, which R11G11B10F reads as 1: it must stay invisible), copies L1 to the game's memory and back into L2, clears a texture L3,
//   copies an RGBA8 checker C2 into L4, and copies a CPU-filled R11G11B10F texture U2 holding every channel code to the game's memory; frame 2 shows L1, L2, L4 and a
//   CPU-filled gradient U in the window's corners. After the frames the CPU reads U2, L2 and L3 back and checks them bit for bit ("b10 check" lines, exit 3 on a
//   failure). Every colour shown is one R11G11B10F holds exactly: how a driver rounds into it (lavapipe truncates, the stand-in rounds to nearest even) cannot show
// GOLDEN_SYNTH_LIT=fine|white adds, after the recorded frame and its checks (both stay as they were), GOLDEN_SYNTH_FRAMES (16) frames like a game's lit scene for
//   the Mali white-screen check (nvn_vk.c mali_white_vk.inc; golden_check.py mali): an R11G11B10F target of 1280x720, cleared, one draw over the part
//   GOLDEN_SYNTH_LIT_SCALE (1: all) of it in a normal colour (fine) or at 65504 (white: what Arm's driver leaves in Sword / Shield's lit scene), then 239 small
//   draws in its centre (240 draws a frame), each frame presented
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
#include "../tex/b10_conv.h"   // (GOLDEN_SYNTH_B10: the packed values the checks expect)

#define SLOG(...) do { fprintf(stderr, "[golden_synth] " __VA_ARGS__); fputc('\n', stderr); } while (0)
enum { WW = 320, WH = 240, AW = 128, CW = 64 };
#define POOL_ADDR 0x3000000000ull
#define POOL_SIZE (32u << 20)
#define HEAP_ADDR 0x3100000000ull
static uint8_t *g_pool_mem; static pool_t *g_pool; static uint8_t *g_heap; static size_t g_heap_used;
static void *heap_alloc_(size_t n) { void *p = g_heap + g_heap_used; g_heap_used += (n + 63) & ~(size_t)63; memset(p, 0, n); return p; }

// ---------- Shaders: keys, map, reflection (the replay's file route) ----------
typedef struct { const char *file, *stage, *vkr; uint8_t key[8]; } shader_def;
static shader_def g_sh[5] = {
    { "flat.vert", "vertex", "# flat.vert: no resources\n", { 'G', 'L', 'D', 'F', 'L', 'T', 'V', 1 } },
    { "flat.frag", "pixel", "# flat.frag: no resources\n", { 'G', 'L', 'D', 'F', 'L', 'T', 'F', 1 } },
    { "lit.vert", "vertex", "b vp_c1 cbuf 1 0 0 0 1\n", { 'G', 'L', 'D', 'L', 'I', 'T', 'V', 1 } },
    { "lit.frag", "pixel", "b fp_c1 cbuf 1 0 0 0 1\nb fp_t0 tex 0 0 0 0 1\n", { 'G', 'L', 'D', 'L', 'I', 'T', 'F', 1 } },
    { "b10.frag", "pixel", "# b10.frag: no resources\n", { 'G', 'L', 'D', 'B', '1', '0', 'F', 1 } },   // (GOLDEN_SYNTH_B10 only)
};
static int g_b10;   // GOLDEN_SYNTH_B10: the R11G11B10F part
static int shaders_setup(const char *spv_in, const char *dir) {
    char p[1400], hex[17]; snprintf(p, sizeof p, "%s/b-%s", dir, DMSH_CHAIN); mkdir(dir, 0755); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/shader_map.tsv", dir); FILE *map = fopen(p, "w"); if (!map) return 0;
    fprintf(map, "package\tindex\tstage\tkey\tsource\n");
    for (int i = 0; i < (g_b10 ? 5 : 4); i++) {
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
// (GOLDEN_SYNTH_B10) a CPU-filled linear texture (rows stride bytes apart), blending, texture copies and clears
static tex_t *mk_tex_lin(int fmt, uint32_t w, uint32_t h, uint32_t stride) {
    tex_t *t = heap_alloc_(sizeof *t);
    t->magic = MAGIC('T', 'E', 'X', 0); t->d.target = NVN_TEXTURE_TARGET_2D; t->d.format = fmt; t->d.width = w; t->d.height = h; t->d.depth = 1; t->d.levels = 1;
    t->d.flags = NVN_TEXTURE_FLAGS_LINEAR; t->d.stride = stride;
    nvn_tex_layout(&t->d, &t->L);
    g_tex_off = (g_tex_off + 0xffff) & ~0xffffull; t->pool = g_pool; t->off = (int64_t)g_tex_off; t->mem = g_pool_mem + g_tex_off; g_tex_off += t->L.size;
    t->serial = nvn_new_serial(); mtl_tex_init(t);
    return t;
}
static void blend0(int on, int src_rgb, int dst_rgb) {   // target 0's blend (alpha: ONE / ZERO, both ADD), and the colour state's blend enable
    p_bind_state b = { 0 }; blend_st bs = { 0, (uint64_t)src_rgb, (uint64_t)dst_rgb, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD }; memcpy(b.bytes, &bs, sizeof bs); cmd(OP_BIND_STATE, &b, sizeof b);
    p_bind_state c = { 1 }; color_st cs = { (uint32_t)(on ? 1 : 0), NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS }; memcpy(c.bytes, &cs, sizeof cs); cmd(OP_BIND_STATE, &c, sizeof c);
}
static void copy_tb(int op, const tex_t *t, uint64_t buf, int w, int h) { p_copy_b2t c; memset(&c, 0, sizeof c); c.src = buf; c.tex = t; c.r.width = w; c.r.height = h; c.r.depth = 1; cmd(op, &c, sizeof c); }
static void copy_tt(const tex_t *s, int sw, int sh, const tex_t *d, int dw, int dh) {
    p_copy_t2t c; memset(&c, 0, sizeof c); c.src = s; c.dst = d; c.sr.width = sw; c.sr.height = sh; c.sr.depth = 1; c.dr.width = dw; c.dr.height = dh; c.dr.depth = 1; cmd(OP_COPY_T2T, &c, sizeof c);
}
static void clear_tex(const tex_t *t, int w, int h, float r, float g, float b) {
    p_clear_tex c; memset(&c, 0, sizeof c); c.tex = t; c.r.width = w; c.r.height = h; c.r.depth = 1; c.mask = 0xf; c.c.f[0] = r; c.c.f[1] = g; c.c.f[2] = b; c.c.f[3] = 1; cmd(OP_CLEAR_TEX, &c, sizeof c);
}
// packed texels against the bits expected, channel by channel, a NaN against any NaN (a GPU may change a NaN's payload); the texels that differ ("b10 check" line)
static int b10_cmp(const char *what, const uint32_t *got, const uint32_t *want, int n) {
    int bad = 0, first = -1;
    for (int i = 0; i < n; i++) { int same = 1;
        for (int c = 0; c < 3; c++) { int mb = c == 2 ? B10_MB : B10_MR, sh = c == 0 ? 0 : c == 1 ? 11 : 22; uint32_t m = c == 2 ? 0x3ff : 0x7ff, a = got[i] >> sh & m, b = want[i] >> sh & m;
            int na = (a >> mb) == 31 && (a & ((1u << mb) - 1)), nb = (b >> mb) == 31 && (b & ((1u << mb) - 1)); if (na || nb ? !(na && nb) : a != b) same = 0; }
        if (!same) { bad++; if (first < 0) first = i; } }
    SLOG("b10 check %s: %s (%d of %d texels differ)", what, bad ? "FAIL" : "PASS", bad, n);
    if (bad) SLOG("  the first at texel %d: %#010x, expected %#010x", first, got[first], want[first]);
    return bad;
}
// a texture's texels read back by the CPU (mtl_read_texels, as nvnTextureReadTexels) against the bits expected
static int b10_check(const char *what, const tex_t *t, int w, int h, const uint32_t *want) {
    uint32_t *got = calloc((size_t)w * h, 4); NVNcopyRegion r = { 0, 0, 0, w, h, 1 }; mtl_read_texels(t, 0, 0, &r, (uint8_t *)got, (size_t)w * 4, (size_t)w * h * 4);
    int bad = b10_cmp(what, got, want, w * h); free(got); return bad;
}

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
    { const char *e = getenv("GOLDEN_SYNTH_B10"); g_b10 = e && atoi(e) > 0; }
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
    // GOLDEN_SYNTH_B10: the R11G11B10F part's objects (L1 lit pass, L2 its copy through the game's memory, L3 a cleared texture, L4 a copy of the RGBA8 checker C2,
    //   U a CPU-filled gradient, U2 every channel code) and memory: a white triangle, the corner quads' vertex blocks, L1's and U2's copies in the game's memory, C2
    enum { BW = 64 }; const uint64_t TRI2 = VB + 0x3000, UBQ = P + 0x1C2000, L1BUF = P + 0x210000, U2BUF = P + 0x220000, CHK2 = P + 0x230000;
    tex_t *L1 = NULL, *L2 = NULL, *L3 = NULL, *L4 = NULL, *U = NULL, *U2 = NULL, *C2 = NULL; prog_t *b10p = NULL; static uint32_t u2_bits[BW * BW];
    if (g_b10) {
        L1 = mk_tex(NVN_FORMAT_R11G11B10F, BW, BW); L2 = mk_tex(NVN_FORMAT_R11G11B10F, BW, BW); L3 = mk_tex(NVN_FORMAT_R11G11B10F, 16, 16); L4 = mk_tex(NVN_FORMAT_R11G11B10F, BW, BW);
        U = mk_tex_lin(NVN_FORMAT_R11G11B10F, BW, BW, BW * 4); U2 = mk_tex_lin(NVN_FORMAT_R11G11B10F, BW, BW, BW * 4); C2 = mk_tex(NVN_FORMAT_RGBA8, BW, BW);
        for (int i = 0; i < BW * BW; i++) { uint8_t *p = (uint8_t *)(uintptr_t)(CHK2 + 4 * (uint64_t)i); int on = ((i % BW >> 3) ^ (i / BW >> 3)) & 1;   // (0 and 255 only: exact in R11G11B10F)
            p[0] = on ? 255 : 0; p[1] = on ? 0 : 255; p[2] = 255; p[3] = 255; }
        for (int y = 0; y < BW; y++) for (int x = 0; x < BW; x++) {   // U: red across, green down, blue 0.5; U2: every R, G and B code (7 and 13 are odd: each walks all of them)
            uint32_t v = b10_float_to_chan(x / (float)(BW - 1), B10_MR) | b10_float_to_chan(y / (float)(BW - 1), B10_MR) << 11 | b10_float_to_chan(0.5f, B10_MB) << 22;
            memcpy(U->mem + 4 * (y * BW + x), &v, 4);
            uint32_t i = (uint32_t)(y * BW + x); u2_bits[i] = (i & 0x7ff) | ((i * 7 + 3) & 0x7ff) << 11 | ((i * 13) & 0x3ff) << 22; memcpy(U2->mem + 4 * i, &u2_bits[i], 4); }
        mtl_reg_tex(3, L1, NULL); mtl_reg_tex(4, L2, NULL); mtl_reg_tex(5, U, NULL); mtl_reg_tex(6, L4, NULL);
        b10p = mk_prog(0, 4);
        float tri2[] = { -1, -1, 0, 1, 1, 1,  1, -1, 0, 1, 1, 1,  0, 1, 0, 1, 1, 1 }; memcpy((void *)(uintptr_t)TRI2, tri2, sizeof tri2);
        for (int k = 0; k < 4; k++) {   // the corner quads' vertex blocks: 64 x 64 pixels in each corner, in front of the background quad
            struct { mat4 mvp, model; float tint[4]; } q = { ident(), ident(), { 1, 1, 1, 1 } };
            q.mvp.m[0] = (float)BW / WW; q.mvp.m[5] = (float)BW / WH; q.mvp.m[12] = (k & 1 ? 1 : -1) * (1 - (float)BW / WW - 0.02f); q.mvp.m[13] = (k & 2 ? 1 : -1) * (1 - (float)BW / WH - 0.03f); q.mvp.m[14] = -0.5f;
            memcpy((void *)(uintptr_t)(UBQ + 0x100 * (uint64_t)k), &q, sizeof q); }
    }

    // ---- frame 1: the triangle into A; the lit state left bound for frame 2 ----
    cmd_begin();
    set_rt(A, NULL); viewport(AW, AW); clear(0.05f, 0.05f, 0.1f); depth_state(0);
    program(flat); vlayout(2, fl_fmt, fl_off, 24); vbuf(VB, sizeof tri);
    { p_draw d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_TRIANGLES; d.count = 3; d.instances = 1; cmd(OP_DRAW, &d, sizeof d); }
    if (g_b10) {   // the R11G11B10F part of frame 1
        // L1, the lit pass: a clear with a negative and a large channel; a triangle in one colour, writing alpha 0.25; a white triangle blended by the target's alpha
        //   (colour = source x (1 - target alpha) + target x target alpha), which R11G11B10F reads as 1: the white must not show
        set_rt(L1, NULL); viewport(BW, BW); clear(-0.5f, 0.3125f, 2.5f); program(b10p);
        { p_draw d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_TRIANGLES; d.count = 3; d.instances = 1; cmd(OP_DRAW, &d, sizeof d);
          program(flat); blend0(1, NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA, NVN_BLEND_FUNC_DST_ALPHA); vbuf(TRI2, 72); cmd(OP_DRAW, &d, sizeof d); blend0(0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO); }
        copy_tb(OP_COPY_T2B, L1, L1BUF, BW, BW);   // L1 into the game's memory and back into L2 (the GPU wrote what the second copy reads)
        copy_tb(OP_COPY_B2T, L2, L1BUF, BW, BW);
        clear_tex(L3, 16, 16, -1.0f, 0.5f, 3.0f);   // (R11G11B10F: the negative channel 0)
        copy_tb(OP_COPY_B2T, C2, CHK2, BW, BW); copy_tt(C2, BW, BW, L4, BW, BW);   // RGBA8 into R11G11B10F: a blit (another format)
        copy_tb(OP_COPY_T2B, U2, U2BUF, BW, BW);    // every channel code into the game's memory
        vbuf(VB, sizeof tri);
    }
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
    if (g_b10) {   // L1, L2, U and L4 in the window's corners (lit, unfiltered)
        static const int tid[4] = { 3, 4, 5, 6 }; vbuf(VB + 0x1000, sizeof quad);
        for (int k = 0; k < 4; k++) { ubo(0, 1, UBQ + 0x100 * (uint64_t)k, 256); tex_handle(1, 0, tid[k], 2); draw_idx(6, IB); }
    }
    submit(cmd_end());
    present(Wt);
    mtl_finish();
    if (g_b10) {   // the CPU's reads, bit for bit: U2 as uploaded; U2 copied into the game's memory by the GPU; L2 as L1's copy left it there; L3 as cleared
        int bad = b10_check("CPU read of a CPU-filled texture holding every channel code", U2, BW, BW, u2_bits);
        bad += b10_cmp("that texture copied into the game's memory", (const uint32_t *)(uintptr_t)U2BUF, u2_bits, BW * BW);
        bad += b10_check("CPU read of a texture copied in from the game's memory (a render target's copy)", L2, BW, BW, (const uint32_t *)(uintptr_t)L1BUF);
        uint32_t want3[16 * 16]; uint32_t c3 = b10_float_to_chan(-1.0f, B10_MR) | b10_float_to_chan(0.5f, B10_MR) << 11 | b10_float_to_chan(3.0f, B10_MB) << 22;
        for (int i = 0; i < 16 * 16; i++) want3[i] = c3;
        bad += b10_check("CPU read of a cleared texture (-1, 0.5, 3)", L3, 16, 16, want3);
        if (bad) { SLOG("b10 checks: %d texels differ", bad); fflush(stderr); _exit(3); }
    }
    {   // GOLDEN_SYNTH_LIT: frames with a lit pass for the white-screen check to sample (the recording above is the same with or without them)
        const char *lm = getenv("GOLDEN_SYNTH_LIT"), *fe = getenv("GOLDEN_SYNTH_FRAMES"), *se = getenv("GOLDEN_SYNTH_LIT_SCALE");
        int white = lm && !strcmp(lm, "white"), nf = fe && atoi(fe) > 0 ? atoi(fe) : 16; float sc = se && atof(se) > 0 && atof(se) <= 1 ? (float)atof(se) : 1.0f;
        if (lm && (white || !strcmp(lm, "fine"))) {
            enum { LW = 1280, LH = 720 }; const uint64_t FST = VB + 0x4000, SMALL = VB + 0x4100;
            tex_t *LIT = mk_tex(NVN_FORMAT_R11G11B10F, LW, LH);
            float r = white ? 65504.0f : 0.25f, g = white ? 65504.0f : 0.5f, b = white ? 65504.0f : 0.75f;
            float fst[18] = { -1, -1, 0, r, g, b,  3, -1, 0, r, g, b,  -1, 3, 0, r, g, b };   // (one triangle over the whole viewport)
            float sm[18] = { -0.01f, -0.01f, 0, 0.5f, 0.5f, 0.5f,  0.01f, -0.01f, 0, 0.5f, 0.5f, 0.5f,  0, 0.01f, 0, 0.5f, 0.5f, 0.5f };
            memcpy((void *)(uintptr_t)FST, fst, sizeof fst); memcpy((void *)(uintptr_t)SMALL, sm, sizeof sm);
            SLOG("lit frames: %d, a %dx%d R11G11B10F target, its %dx%d part drawn %s, 240 draws a frame", nf, LW, LH, (int)(LW * sc), (int)(LH * sc), white ? "at 65504 (white)" : "in a normal colour (fine)");
            for (int f = 0; f < nf; f++) {
                cmd_begin();
                set_rt(LIT, NULL); viewport((int)(LW * sc), (int)(LH * sc)); clear(0.1f, 0.2f, 0.3f); depth_state(0);
                program(flat); vlayout(2, fl_fmt, fl_off, 24); vbuf(FST, sizeof fst);
                p_draw d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_TRIANGLES; d.count = 3; d.instances = 1; cmd(OP_DRAW, &d, sizeof d);
                vbuf(SMALL, sizeof sm); for (int k = 0; k < 239; k++) cmd(OP_DRAW, &d, sizeof d);
                submit(cmd_end());
                present(Wt);
            }
            mtl_finish();
            SLOG("lit frames done");
        }
    }
    char rec[1400]; snprintf(rec, sizeof rec, "%s/%s-synth-p2.nxgold", outdir, title);
    struct stat st; if (stat(rec, &st)) { SLOG("no recording at %s", rec); _exit(1); }
    SLOG("recorded %s (%lld bytes); the live picture is %s-synth-p2.phone.png", rec, (long long)st.st_size, title);
    fflush(stderr); _exit(0);
}
