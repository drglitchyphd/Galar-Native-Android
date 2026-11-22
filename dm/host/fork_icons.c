// FORK v61: Pokémon icons for the info panels (Steven, Nov 19: "icons pulled from the party / dex icons"), read from the player's own RomFS, never shipped.
//   The game's party / box icons are BNTX textures in bin/appli/icon_pokemon/ (poke_icon_<species 4>_<form 3>_..._<n normal / r shiny>.bntx; when that folder is
//   not there, RomFS is searched for poke_icon_ files and the folder found is logged). A requested icon is decoded on the area dex's thread (fork_dex.c):
//   BNTX → BRTI (format, size, block height) → Tegra block-linear to linear (gfx/tex/nvn_deswizzle.c nvn_bl_offset) → BCn to RGBA (gfx/tex/bc_decode.c) → scaled
//   into a 64 x 64 cell of a 1024 x 1024 atlas, which the overlay samples (gfx/nvn/nvn_vk.c, binding 1; overlay/ovl.frag: glyph 1000 + cell). Logs "[FORK] icons".
#include "hle.h"
#include <dirent.h>
#include <sys/stat.h>
#include "../gfx/tex/bc_decode.h"
uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td);   // gfx/tex/nvn_deswizzle.c

enum { AW = 1024, CELL = 64, NCELL = 255 };
static uint8_t *g_atlas; static volatile unsigned g_gen;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { int16_t sp, form; int8_t st; } g_cell[NCELL]; static int g_ncell;   // st: 0 asked, 1 drawn, -1 no icon
static char g_dir[1024]; static int g_dir_tried;
static char (*g_files)[96]; static int g_nfiles;
static char (*g_inner)[64];   // v65: the name of the texture inside each file (Steven, Nov 22: the file numbers are not the Pokémon: poke_icon_0819 holds Mimikyu)
static int g_lines;
#define ILOG(...) do { if (g_lines < 80) { g_lines++; LOG(__VA_ARGS__); } } while (0)

const uint8_t *dm_fork_icons(unsigned *gen) { *gen = g_gen; return g_atlas; }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void romfs_path(char *buf, size_t n, const char *rel) {   // as hle_fs.c envdir("DM_ROMFS", "romfs")
    const char *r = getenv("DM_ROMFS");
    if (r) snprintf(buf, n, "%s/%s", r, rel);
    else snprintf(buf, n, "%s%s/romfs/%s", getenv("DM_ROOT") ? getenv("DM_ROOT") : getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work", rel);
}
static int find_dir(const char *d, int depth) {   // a folder holding poke_icon_*.bntx
    DIR *D = opendir(d); if (!D) return 0; struct dirent *e; int hit = 0; char sub[64][256]; int ns = 0;
    while ((e = readdir(D))) { if (e->d_name[0] == '.') continue;
        if (!strncmp(e->d_name, "poke_icon_", 10) && strstr(e->d_name, ".bntx")) { hit = 1; break; }
        if (depth > 0 && ns < 64 && !strchr(e->d_name, '.')) snprintf(sub[ns++], sizeof sub[0], "%s", e->d_name); }
    closedir(D);
    if (hit) { snprintf(g_dir, sizeof g_dir, "%s", d); return 1; }
    for (int i = 0; i < ns; i++) { char p[1024]; snprintf(p, sizeof p, "%s/%s", d, sub[i]); struct stat st; if (!stat(p, &st) && S_ISDIR(st.st_mode) && find_dir(p, depth - 1)) return 1; }
    return 0;
}
static uint8_t *read_file(const char *path, size_t *n);
// the first texture's name in a BNTX (BRTI +0x60: a string entry, u16 length then the characters)
static int bntx_name(const uint8_t *d, size_t n, char *out, size_t on) {
    out[0] = 0; if (n < 0x40 || memcmp(d, "BNTX", 4)) return 0;
    const uint8_t *nx = NULL; for (size_t o = 0x20; o + 0x28 <= n && o < 0x100; o += 4) if (!memcmp(d + o, "NX  ", 4)) { nx = d + o; break; }
    if (!nx || rd32(nx + 4) < 1) return 0;
    uint64_t pa = rd64(nx + 8); if (pa + 8 > n) return 0; uint64_t bi = rd64(d + pa); if (bi + 0x78 > n || memcmp(d + bi, "BRTI", 4)) return 0;
    uint64_t np = rd64(d + bi + 0x60); if (np + 2 > n) return 0; size_t L = rd16(d + np); if (np + 2 + L > n || L >= on) return 0;
    memcpy(out, d + np + 2, L); out[L] = 0; return 1;
}
static void index_dir(void) {
    g_dir_tried = 1; char p[1024]; romfs_path(p, sizeof p, "bin/appli/icon_pokemon");
    if (!find_dir(p, 0)) { romfs_path(p, sizeof p, "bin"); if (!find_dir(p, 4)) { ILOG("[FORK] icons: no poke_icon_*.bntx found in RomFS: no icons"); return; } }
    DIR *D = opendir(g_dir); struct dirent *e; int cap = 0;
    while (D && (e = readdir(D))) { if (strncmp(e->d_name, "poke_icon_", 10) || strlen(e->d_name) >= 96) continue;
        if (g_nfiles == cap) { cap = cap ? cap * 2 : 2048; void *n = realloc(g_files, (size_t)cap * 96); if (!n) break; g_files = n; }
        snprintf(g_files[g_nfiles++], 96, "%s", e->d_name); }
    if (D) closedir(D);
    ILOG("[FORK] icons: %d icon files in %s", g_nfiles, g_dir);
    // v65: each file's texture name (it says which Pokémon it is); icon_file matches on it first
    g_inner = calloc((size_t)(g_nfiles ? g_nfiles : 1), 64); int named = 0, differ = 0; uint64_t t0 = now_ns();
    for (int i = 0; g_inner && i < g_nfiles; i++) { char p2[1200]; snprintf(p2, sizeof p2, "%s/%s", g_dir, g_files[i]); size_t n = 0; uint8_t *d = read_file(p2, &n);
        if (d && bntx_name(d, n, g_inner[i], 64)) { named++; if (strncmp(g_inner[i], g_files[i], strlen(g_inner[i])) && differ++ < 12) ILOG("[FORK] icons: %s holds texture %s", g_files[i], g_inner[i]); }
        free(d); }
    ILOG("[FORK] icons: %d texture names read, %d differ from their file's name (%.1f s)", named, differ, (now_ns() - t0) / 1e9);
    for (int i = 0; i < g_nfiles && i < 16; i++) ILOG("[FORK] icons: file %s", g_files[i * (g_nfiles / 16 + 1) % g_nfiles]);   // (v62: a spread of names)
}
// the file for a species / form. v62: Sword names them poke_icon_SSSS_FFg_x.bntx (Steven's v61 log: "poke_icon_0000_00s_n.bntx": a 2-digit form, then the
//   gender (s both / m / f), then n (normal; g Gigantamax?)): the m / s one, then any
static const char *icon_file(int sp, int form) {
    char pre[32]; snprintf(pre, sizeof pre, "poke_icon_%04d_%02d", sp, form); size_t L = strlen(pre); const char *best = NULL; int bs = -1;
    if (g_inner) {   // v65: by the texture's own name (the same pattern), when the files carry names
        for (int i = 0; i < g_nfiles; i++) { const char *f = g_inner[i]; if (strncmp(f, pre, L)) continue; char gch = f[L]; if (gch >= '0' && gch <= '9') continue;
            int s = 0; size_t n = strlen(f); if (n > 2 && !strcmp(f + n - 2, "_n")) s += 4; if (gch == 's' || gch == 'm') s += 2;
            if (s > bs) { bs = s; best = g_files[i]; } }
        if (best) return best; }
    for (int i = 0; i < g_nfiles; i++) { const char *f = g_files[i]; if (strncmp(f, pre, L)) continue;
        char gch = f[L]; if (gch >= '0' && gch <= '9') continue;   // (a longer form number: another form)
        int s = 0; size_t n = strlen(f); if (n > 7 && !strcmp(f + n - 7, "_n.bntx")) s += 4; if (gch == 's' || gch == 'm') s += 2;
        if (s > bs) { bs = s; best = f; } }
    return best;
}
static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = L > 0 ? malloc((size_t)L) : NULL; if (b && fread(b, 1, (size_t)L, f) != (size_t)L) { free(b); b = NULL; } fclose(f); if (b) *n = (size_t)L; return b;
}
// BNTX's first texture, level 0, as RGBA8 (malloc'd), or NULL
static uint8_t *bntx_rgba(const uint8_t *d, size_t n, int *W, int *H) {
    if (n < 0x40 || memcmp(d, "BNTX", 4)) return NULL;
    const uint8_t *nx = NULL; for (size_t o = 0x20; o + 0x28 <= n && o < 0x100; o += 4) if (!memcmp(d + o, "NX  ", 4)) { nx = d + o; break; }
    if (!nx || rd32(nx + 4) < 1) return NULL;
    uint64_t pa = rd64(nx + 8); if (pa + 8 > n) return NULL; uint64_t bi = rd64(d + pa); if (bi + 0x78 > n || memcmp(d + bi, "BRTI", 4)) return NULL;
    const uint8_t *t = d + bi; int tile = rd16(t + 0x12); uint32_t fmt = rd32(t + 0x1C); int w = (int)rd32(t + 0x24), h = (int)rd32(t + 0x28); int bhl = (int)(rd32(t + 0x34) & 7);
    uint64_t mp = rd64(t + 0x70); if (mp + 8 > n) return NULL; uint64_t data = rd64(d + mp);
    int ft = (int)(fmt >> 8), kind = ft == 0x1A ? 1 : ft == 0x1B ? 2 : ft == 0x1C ? 3 : ft == 0x20 ? 7 : ft == 0x0B ? 0 : -1;
    static int said; if (!said++) ILOG("[FORK] icons: first icon: %dx%d, format %#x, tile mode %d, block height 2^%d", w, h, fmt, tile, bhl);
    if (kind < 0 || w <= 0 || h <= 0 || w > 512 || h > 512) { ILOG("[FORK] icons: format %#x (%dx%d) not read", fmt, w, h); return NULL; }
    int bw = kind ? 4 : 1, bpb = kind ? bc_block_bytes(kind) : 4; uint32_t rowb = (uint32_t)((w + bw - 1) / bw * bpb), rows = (uint32_t)((h + bw - 1) / bw);
    uint8_t *lin = malloc((size_t)rowb * rows); if (!lin) return NULL;
    for (uint32_t y = 0; y < rows; y++) for (uint32_t x = 0; x < rowb; x += 16 < bpb ? 16 : (uint32_t)bpb) {   // (16-byte runs stay inside one GOB line)
        uint32_t run = rowb - x < 16 ? rowb - x : 16; uint64_t o = tile == 1 ? (uint64_t)y * rowb + x : nvn_bl_offset(x, y, 0, rowb, rows, (unsigned)bhl, 0);
        if (data + o + run > n) { free(lin); return NULL; } memcpy(lin + (size_t)y * rowb + x, d + data + o, run); }
    uint8_t *out = malloc((size_t)w * h * 4); if (!out) { free(lin); return NULL; }
    if (kind) bc_decode_image(kind, lin, rowb, w, h, out, (size_t)w * 4); else for (int y = 0; y < h; y++) memcpy(out + (size_t)y * w * 4, lin + (size_t)y * rowb, (size_t)w * 4);
    free(lin); *W = w; *H = h; return out;
}
static void draw_cell(int c, const uint8_t *px0, int W0, int H0) {   // fitted into the cell, aspect kept, box-filtered
    // v63 (Steven, Nov 20: "really tiny"): cropped to the Pokémon first (the pixels not see-through), a square around it
    int bx0 = W0, by0 = H0, bx1 = -1, by1 = -1;
    for (int y = 0; y < H0; y++) for (int x = 0; x < W0; x++) if (px0[((size_t)y * W0 + x) * 4 + 3] > 16) { if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y; }
    if (bx1 < 0) { bx0 = by0 = 0; bx1 = W0 - 1; by1 = H0 - 1; }
    int w = bx1 - bx0 + 1, h = by1 - by0 + 1; const uint8_t *px = px0 + ((size_t)by0 * W0 + bx0) * 4; int pitch = W0;
    int cx = (c % 16) * CELL, cy = (c / 16) * CELL; float s = (float)(w > h ? w : h) / (CELL - 2); int ow = (int)(w / s), oh = (int)(h / s), x0 = (CELL - ow) / 2, y0 = (CELL - oh) / 2;
    for (int y = 0; y < CELL; y++) memset(g_atlas + ((size_t)(cy + y) * AW + cx) * 4, 0, CELL * 4);
    for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
        int sx0 = (int)(x * s), sy0 = (int)(y * s), sx1 = (int)((x + 1) * s), sy1 = (int)((y + 1) * s); if (sx1 <= sx0) sx1 = sx0 + 1; if (sy1 <= sy0) sy1 = sy0 + 1;
        unsigned acc[4] = { 0 }, k = 0;
        for (int yy = sy0; yy < sy1 && yy < h; yy++) for (int xx = sx0; xx < sx1 && xx < w; xx++) { const uint8_t *q = px + ((size_t)yy * pitch + xx) * 4; unsigned a = q[3]; acc[0] += q[0] * a; acc[1] += q[1] * a; acc[2] += q[2] * a; acc[3] += a; k++; }
        uint8_t *o = g_atlas + ((size_t)(cy + y0 + y) * AW + cx + x0 + x) * 4;
        if (acc[3]) { o[0] = (uint8_t)(acc[0] / acc[3]); o[1] = (uint8_t)(acc[1] / acc[3]); o[2] = (uint8_t)(acc[2] / acc[3]); } o[3] = (uint8_t)(k ? acc[3] / k : 0); }
}
// the cell of an icon (1000 + cell is the overlay's glyph), -1 not yet / none; asks for it
int dm_fork_icon_cell(int sp, int form) {
    if (sp <= 0) return -1; pthread_mutex_lock(&g_mu); int r = -1, i = 0;
    for (; i < g_ncell; i++) if (g_cell[i].sp == sp && g_cell[i].form == form) { r = g_cell[i].st == 1 ? i : -1; break; }
    if (i == g_ncell && g_ncell < NCELL) { g_cell[g_ncell].sp = (int16_t)sp; g_cell[g_ncell].form = (int16_t)form; g_cell[g_ncell].st = 0; g_ncell++; }
    pthread_mutex_unlock(&g_mu); return r;
}
// on the area dex's thread: draws up to 8 asked-for icons
// v64: the decoded icons, once (Steven, Nov 22: icons show but are the wrong Pokémon; the file names match): the atlas's first two rows of cells as a BMP in
//   Download (next to the log), and the first icon as decoded (before the crop). Read-only for the game; the picture is the player's own icons
static void bmp(const char *path, const uint8_t *rgba, int w, int h, int pitch) {
    FILE *f = fopen(path, "wb"); if (!f) { ILOG("[FORK] icons: %s not writable", path); return; }
    uint32_t sz = (uint32_t)(54 + w * h * 4); uint8_t hd[54] = { 'B', 'M' }; memcpy(hd + 2, &sz, 4); uint32_t o = 54, hs = 40, img = (uint32_t)(w * h * 4); int32_t W = w, H = -h; uint16_t pl = 1, bpp = 32;
    memcpy(hd + 10, &o, 4); memcpy(hd + 14, &hs, 4); memcpy(hd + 18, &W, 4); memcpy(hd + 22, &H, 4); memcpy(hd + 26, &pl, 2); memcpy(hd + 28, &bpp, 2); memcpy(hd + 34, &img, 4);
    fwrite(hd, 1, 54, f);
    uint8_t *row = malloc((size_t)w * 4); for (int y = 0; y < h && row; y++) { const uint8_t *q = rgba + (size_t)y * pitch * 4;
        for (int x = 0; x < w; x++) { uint8_t a = q[x * 4 + 3]; row[x * 4] = (uint8_t)(q[x * 4 + 2] * a / 255 + (255 - a) * (((x / 8 + y / 8) & 1) ? 200 : 255) / 255); row[x * 4 + 1] = (uint8_t)(q[x * 4 + 1] * a / 255 + (255 - a) * (((x / 8 + y / 8) & 1) ? 200 : 255) / 255);
            row[x * 4 + 2] = (uint8_t)(q[x * 4] * a / 255 + (255 - a) * (((x / 8 + y / 8) & 1) ? 200 : 255) / 255); row[x * 4 + 3] = 255; }   // (on a checkerboard: see-through shows)
        fwrite(row, 1, (size_t)w * 4, f); }
    free(row); fclose(f); ILOG("[FORK] icons: wrote %s (%dx%d)", path, w, h);
}
static void dl_path(char *b, size_t n, const char *name) { snprintf(b, n, "/storage/emulated/0/Download/%s", name); }
static int g_dumped, g_first_dumped;
int dm_fork_devno(int sp, int form);   // fork_dex.c (v66)
void dm_fork_icons_work(void) {
    if (!g_dir_tried) index_dir(); if (!g_nfiles) return;
    if (!g_atlas && !(g_atlas = calloc((size_t)AW * AW, 4))) return;
    for (int k = 0; k < 8; k++) {
        int c = -1, sp = 0, form = 0; pthread_mutex_lock(&g_mu); for (int i = 0; i < g_ncell; i++) if (g_cell[i].st == 0) { c = i; sp = g_cell[i].sp; form = g_cell[i].form; break; } pthread_mutex_unlock(&g_mu);
        if (c < 0) { if (!g_dumped && g_ncell >= 8) { g_dumped = 1; char bp[256]; dl_path(bp, sizeof bp, "GalarFork-icons.bmp"); bmp(bp, g_atlas, AW, CELL * 2, AW); } return; }   // v64: the atlas, once
        int dv = dm_fork_devno(sp, form), dv0 = dm_fork_devno(sp, 0);   // v66: files named by the game's own number (personal +0x4C)
        const char *f = dv > 0 ? icon_file(dv, form) : NULL; if (!f && dv0 > 0 && form) f = icon_file(dv0, form);
        if (!f) f = icon_file(sp, form); if (!f && form) f = icon_file(sp, 0);
        ILOG("[FORK] icons: species %d form %d: game number %d", sp, form, dv);
        int ok = 0; if (f) { char p[1200]; snprintf(p, sizeof p, "%s/%s", g_dir, f); size_t n = 0; uint8_t *d = read_file(p, &n), *px = NULL; int w = 0, h = 0;
            if (d) px = bntx_rgba(d, n, &w, &h); free(d);
            if (px && !g_first_dumped) { g_first_dumped = 1; char bp[256]; dl_path(bp, sizeof bp, "GalarFork-icon-first.bmp"); bmp(bp, px, w, h, w); ILOG("[FORK] icons: that one is species %d form %d (%s)", sp, form, f); }   // v64
            if (px) { draw_cell(c, px, w, h); free(px); ok = 1; } }
        if (!ok) ILOG("[FORK] icons: no icon for species %d form %d%s", sp, form, f ? " (file not read)" : "");
        else ILOG("[FORK] icons: species %d form %d <- %s (cell %d)", sp, form, f, c);   // v63 (Steven: the wrong Pokémon: which file each one got)
        pthread_mutex_lock(&g_mu); g_cell[c].st = ok ? 1 : -1; pthread_mutex_unlock(&g_mu);
        if (ok) g_gen++;
    }
}
