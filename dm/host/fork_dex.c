// FORK v55: the area dex (touch_ui.c dex_draw, DM_FORK_AREADEX=on, default off): the Pokémon of the area the player is in, how often they appear, and whether the
//   player's Pokédex has them seen / caught. Read-only: nothing in the game, its files or the save is written. Logs "[FORK] dex ...".
// Where each part comes from (docs/ENCOUNTER_INFO_RESEARCH.md, "Area dex"):
//   - the spawns: the player's own RomFS (read-only), bin/archive/field/resident/data_table.gfpak: encount_symbol_k.bin (the Pokémon walking about) and encount_k.bin
//     (the hidden ones: grass "!", shaking trees, fishing); _t for Shield. GFLXPACK (pkNX GFPack.cs, entries found by the FNV-1a 64 hash of their file name, LZ4 block
//     compressed) holding FlatBuffers (pkNX EncounterArchive.fbs: tables of ZoneID u64 + 11 subtables, one per weather / kind, of slots: probability, species, form)
//   - zone names: fork_zones.h (pkNX SWSHInfo.cs); zones of one place (" (Surfing)", " (Flying)", ...) are shown together under the place's name
//   - the Pokédex: the save's KZukan / KZukanR1 / KZukanR2 blocks (PKHeX Zukan8: 0x30 per entry; seen = any of the four u64 at +0, caught = bit 0 of the u32 at +0x20),
//     the species' dex numbers from RomFS bin/pml/personal/personal_total.bin (0xB0 per species: Galar +0x5C, Armor +0xAC, Crown +0xAE). The blocks are found in
//     memory by their contents: the save file ($DM_SAVE/main, PKHeX SwishCrypto: static pad, xorshift per block) is decoded and its blocks looked for in the heap
//   - where the player is: unknown in memory. KCoordinates' map hash (heap + 0x450613C0, Sys-EncounterBot) is only written when the game saves. So a probe: the heap's
//     values that are zone hashes are listed, and each time the area name popup comes up (gfx/nvn/fork_ui.h dm_fork_area_popups) the ones that changed to another
//     zone hash are the candidates; one that changes with two popups in a row is kept and read live. Until then the last changed one, else the saved map hash
#include "hle.h"
#include <stdatomic.h>
#include "fork_zones.h"
uintptr_t dm_fork_heap_base(void);   // hle_svc.c (FORK)
size_t dm_fork_heap_size(void);      // hle_svc.c (FORK)
int dm_guest_heap_readable(uint64_t a, size_t n);   // hle_svc.c

typedef struct { int16_t species, form; uint8_t pct, state, flags, pad; } dm_fork_dex_row;   // state: 0 not seen, 1 seen, 2 caught; flags 1 walking about, 2 hidden
typedef struct { int active, n, weather, dexok; char area[32]; dm_fork_dex_row row[40]; char msg[2][24]; } dm_fork_dex_info;   // (v56: msg: what the panel waits for, shown instead of the list)   // weather: -1 all, else the subtable (0-10)
extern volatile int dm_fork_dex_on __attribute__((weak));   // touch_ui.c
extern _Atomic int dm_fork_area_popups __attribute__((weak));   // gfx/nvn/fork_ui.h

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static int g_lines;
#define DLOG(...) do { if (g_lines < 200) { g_lines++; LOG(__VA_ARGS__); } } while (0)

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = L > 0 ? malloc((size_t)L) : NULL;
    if (b && fread(b, 1, (size_t)L, f) != (size_t)L) { free(b); b = NULL; }
    fclose(f); if (b) *n = (size_t)L; return b;
}
static void romfs_path(char *buf, size_t n, const char *rel) {   // as hle_fs.c envdir("DM_ROMFS", "romfs")
    const char *r = getenv("DM_ROMFS");
    if (r) snprintf(buf, n, "%s/%s", r, rel);
    else snprintf(buf, n, "%s%s/romfs/%s", getenv("DM_ROOT") ? getenv("DM_ROOT") : getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work", rel);
}
static int is_sword(void) {
    static int v = -1; if (v >= 0) return v;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main") && g_mod[i].build_id[0]) { v = !strcasecmp(g_mod[i].build_id, "a3b75bcd3311385aeed67fbeeb79cbb7"); return v; }
    return 0;
}

// ---------- the spawn tables ----------
static int lz4_block(const uint8_t *s, size_t sn, uint8_t *d, size_t dn) {
    size_t i = 0, o = 0;
    while (i < sn) {
        uint8_t t = s[i++]; size_t l = t >> 4;
        if (l == 15) { uint8_t b; do { if (i >= sn) return -1; b = s[i++]; l += b; } while (b == 255); }
        if (i + l > sn || o + l > dn) return -1;
        memcpy(d + o, s + i, l); i += l; o += l;
        if (i >= sn) break;
        if (i + 2 > sn) return -1;
        size_t off = s[i] | (size_t)s[i + 1] << 8; i += 2; if (!off || off > o) return -1;
        size_t m = t & 15; if (m == 15) { uint8_t b; do { if (i >= sn) return -1; b = s[i++]; m += b; } while (b == 255); } m += 4;
        if (o + m > dn) return -1;
        for (size_t k = 0; k < m; k++, o++) d[o] = d[o - off];
    }
    return (int)o;
}
static uint64_t fnv1a64(const char *s) { uint64_t h = 0xCBF29CE484222645ull; for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x100000001B3ull; } return h; }
// one file of a GFLXPACK by its file name: malloc'd, decompressed; NULL if not there
static uint8_t *gfpak_file(const uint8_t *d, size_t n, const char *name, size_t *out) {
    if (n < 0x28 || memcmp(d, "GFLXPACK", 8)) return NULL;
    int nf = (int)rd32(d + 0x10), nd = (int)rd32(d + 0x14); uint64_t ftab = rd64(d + 0x18), h = fnv1a64(name);
    if (nf <= 0 || nd < 0 || 0x28 + (size_t)nd * 8 > n || ftab + (uint64_t)nf * 0x18 > n) return NULL;
    for (int f = 0; f < nd; f++) {
        uint64_t p = rd64(d + 0x28 + (size_t)f * 8); if (p + 0x10 > n) return NULL;
        int cnt = (int)rd32(d + p + 8); if (cnt < 0 || p + 0x10 + (uint64_t)cnt * 0x10 > n) return NULL;
        for (int i = 0; i < cnt; i++) {
            const uint8_t *e = d + p + 0x10 + (size_t)i * 0x10; if (rd64(e) != h) continue;
            int idx = (int)rd32(e + 8); if (idx < 0 || idx >= nf) return NULL;
            const uint8_t *fe = d + ftab + (size_t)idx * 0x18; int typ = fe[2]; uint32_t us = rd32(fe + 4), cs = rd32(fe + 8), off = rd32(fe + 0x10);
            if ((uint64_t)off + cs > n || !us || us > (64u << 20)) return NULL;
            uint8_t *o = malloc(us); if (!o) return NULL;
            if (typ == 0 && cs == us) memcpy(o, d + off, us);
            else if (typ != 2 || lz4_block(d + off, cs, o, us) != (int)us) { DLOG("[FORK] dex: %s: compression %d not read", name, typ); free(o); return NULL; }
            *out = us; return o; }
    }
    return NULL;
}
// FlatBuffers, bounds-checked: a table's field position (-1 absent), an offset followed
typedef struct { const uint8_t *b; size_t n; } fbuf;
static int fb_in(const fbuf *f, int64_t o, size_t len) { return o >= 0 && (uint64_t)o <= f->n && len <= f->n - (size_t)o; }
static int64_t fb_field(const fbuf *f, int64_t t, int i) {
    if (!fb_in(f, t, 4)) return -1; int64_t vt = t - (int32_t)rd32(f->b + t); if (!fb_in(f, vt, 4)) return -1;
    uint16_t vs = rd16(f->b + vt); if (4 + 2 * i + 2 > vs || !fb_in(f, vt, vs)) return -1;
    uint16_t fo = rd16(f->b + vt + 4 + 2 * i); return fo ? t + fo : -1;
}
static int64_t fb_ref(const fbuf *f, int64_t p) { if (!fb_in(f, p, 4)) return -1; int64_t r = p + rd32(f->b + p); return fb_in(f, r, 4) ? r : -1; }
static uint32_t fb_vlen(const fbuf *f, int64_t v) { uint32_t L = rd32(f->b + v); return fb_in(f, v + 4, (size_t)L * 4) ? L : 0; }

typedef struct { uint16_t zone; uint8_t kind, sub; uint16_t species; uint8_t form, prob; } slot;   // zone: index in k_zones; kind 0 walking about, 1 hidden
static slot *g_slots; static int g_nslots, g_slots_cap;
enum { NZONES = sizeof k_zones / sizeof *k_zones };
static int zone_index(uint64_t h) { int lo = 0, hi = NZONES - 1; while (lo <= hi) { int m = (lo + hi) / 2; if (k_zones[m].hash == h) return m; if (k_zones[m].hash < h) lo = m + 1; else hi = m - 1; } return -1; }
static int load_archive(const uint8_t *b, size_t n, int kind) {
    fbuf f = { b, n }; if (n < 8) return 0; int64_t root = rd32(b); int tabs = 0, unknown = 0;
    int64_t tv = fb_ref(&f, fb_field(&f, root, 1)); if (tv < 0) return 0;
    for (uint32_t i = 0, nt = fb_vlen(&f, tv); i < nt; i++) {
        int64_t t = fb_ref(&f, tv + 4 + 4 * (int64_t)i), zf = fb_field(&f, t, 0); if (t < 0 || !fb_in(&f, zf, 8)) continue;
        int z = zone_index(rd64(b + zf)); if (z < 0) { unknown++; continue; } tabs++;
        int64_t sv = fb_ref(&f, fb_field(&f, t, 1)); if (sv < 0) continue;
        for (uint32_t s = 0, ns = fb_vlen(&f, sv); s < ns && s < 11; s++) {
            int64_t st = fb_ref(&f, sv + 4 + 4 * (int64_t)s), lv = fb_ref(&f, fb_field(&f, st, 2)); if (st < 0 || lv < 0) continue;
            for (uint32_t k = 0, nk = fb_vlen(&f, lv); k < nk; k++) {
                int64_t e = fb_ref(&f, lv + 4 + 4 * (int64_t)k); if (e < 0) continue;
                int64_t pf = fb_field(&f, e, 0), sf = fb_field(&f, e, 1), ff = fb_field(&f, e, 2);
                int sp = fb_in(&f, sf, 4) ? (int)rd32(b + sf) : 0, pr = fb_in(&f, pf, 1) ? b[pf] : 0; if (sp <= 0 || sp > 898 || !pr) continue;
                if (g_nslots == g_slots_cap) { int c = g_slots_cap ? g_slots_cap * 2 : 4096; slot *ns2 = realloc(g_slots, (size_t)c * sizeof *ns2); if (!ns2) return tabs; g_slots = ns2; g_slots_cap = c; }
                g_slots[g_nslots++] = (slot){ (uint16_t)z, (uint8_t)kind, (uint8_t)s, (uint16_t)sp, fb_in(&f, ff, 1) ? b[ff] : 0, (uint8_t)pr }; } } }
    if (unknown) DLOG("[FORK] dex: %d tables of zones without a name left out", unknown);
    return tabs;
}
static int load_tables(void) {
    char p[1024]; romfs_path(p, sizeof p, "bin/archive/field/resident/data_table.gfpak");
    size_t n = 0; uint8_t *d = read_file(p, &n); if (!d) { DLOG("[FORK] dex: %s not readable: no spawn tables", p); return 0; }
    const char *nm[2] = { is_sword() ? "encount_symbol_k.bin" : "encount_symbol_t.bin", is_sword() ? "encount_k.bin" : "encount_t.bin" };
    for (int k = 0; k < 2; k++) { size_t m = 0; uint8_t *a = gfpak_file(d, n, nm[k], &m);
        if (!a) { DLOG("[FORK] dex: %s not found in data_table.gfpak", nm[k]); continue; }
        int before = g_nslots, t = load_archive(a, m, k); free(a);
        DLOG("[FORK] dex: %s: %d zones, %d slots", nm[k], t, g_nslots - before); }
    free(d); return g_nslots > 0;
}

// ---------- the Pokédex ----------
static uint8_t g_dexblk[899]; static uint16_t g_dexidx[899];   // per species: block (0 Galar, 1 Armor, 2 Crown) and dex number (0: none)
static int load_personal(void) {
    char p[1024]; romfs_path(p, sizeof p, "bin/pml/personal/personal_total.bin");
    size_t n = 0; uint8_t *d = read_file(p, &n); if (!d || n < 899 * 0xB0) { DLOG("[FORK] dex: %s not readable", p); free(d); return 0; }
    int c = 0; for (int s = 1; s < 899; s++) { const uint8_t *e = d + (size_t)s * 0xB0; uint16_t g = rd16(e + 0x5C), a = rd16(e + 0xAC), cr = rd16(e + 0xAE);
        if (g) { g_dexblk[s] = 0; g_dexidx[s] = g; } else if (a) { g_dexblk[s] = 1; g_dexidx[s] = a; } else if (cr) { g_dexblk[s] = 2; g_dexidx[s] = cr; } else continue; c++; }
    free(d); DLOG("[FORK] dex: %d species with a dex number", c); return c > 0;
}
static const uint8_t k_pad[0x7F] = {
    0xA0, 0x92, 0xD1, 0x06, 0x07, 0xDB, 0x32, 0xA1, 0xAE, 0x01, 0xF5, 0xC5, 0x1E, 0x84, 0x4F, 0xE3, 0x53, 0xCA, 0x37, 0xF4, 0xA7, 0xB0, 0x4D, 0xA0, 0x18, 0xB7, 0xC2, 0x97,
    0xDA, 0x5F, 0x53, 0x2B, 0x75, 0xFA, 0x48, 0x16, 0xF8, 0xD4, 0x8A, 0x6F, 0x61, 0x05, 0xF4, 0xE2, 0xFD, 0x04, 0xB5, 0xA3, 0x0F, 0xFC, 0x44, 0x92, 0xCB, 0x32, 0xE6, 0x1B,
    0xB9, 0xB1, 0x2E, 0x01, 0xB0, 0x56, 0x53, 0x36, 0xD2, 0xD1, 0x50, 0x3D, 0xDE, 0x5B, 0x2E, 0x0E, 0x52, 0xFD, 0xDF, 0x2F, 0x7B, 0xCA, 0x63, 0x50, 0xA4, 0x67, 0x5D, 0x23,
    0x17, 0xC0, 0x52, 0xE1, 0xA6, 0x30, 0x7C, 0x2B, 0xB6, 0x70, 0x36, 0x5B, 0x2A, 0x27, 0x69, 0x33, 0xF5, 0x63, 0x7B, 0x36, 0x3F, 0x26, 0x9B, 0xA3, 0xED, 0x7A, 0x53, 0x00,
    0xA4, 0x48, 0xB3, 0x50, 0x9E, 0x14, 0xA0, 0x52, 0xDE, 0x7E, 0x10, 0x2B, 0x1B, 0x77, 0x6E };   // PKHeX SwishCrypto.StaticXorpad (period 0x7F)
typedef struct { uint32_t s; int c; } xs32;
static uint32_t xs_adv(uint32_t s) { s ^= s << 2; s ^= s >> 15; s ^= s << 13; return s; }
static xs32 xs_init(uint32_t key) { xs32 x = { key, 0 }; for (int i = __builtin_popcount(key); i > 0; i--) x.s = xs_adv(x.s); return x; }
static uint8_t xs_next(xs32 *x) { uint8_t r = (uint8_t)(x->s >> (x->c * 8)); if (x->c == 3) { x->s = xs_adv(x->s); x->c = 0; } else x->c++; return r; }
static int type_size(int t) { return t == 3 || t == 8 || t == 12 ? 1 : t == 9 || t == 13 ? 2 : t == 10 || t == 14 || t == 16 ? 4 : t == 11 || t == 15 || t == 17 ? 8 : -1; }
static const uint32_t k_zkey[3] = { 0x4716C404u, 0x3F936BA9u, 0x3C9366F0u };   // KZukan, KZukanR1 (Armor), KZukanR2 (Crown)
static uint8_t *g_zsave[3]; static size_t g_zlen[3]; static uint64_t g_zoff[3];   // the save's blocks; where they are in the heap (0: not found)
static int load_save(void) {
    const char *sd = getenv("DM_SAVE"); if (!sd) return 0;
    char p[1024]; snprintf(p, sizeof p, "%s/main", sd); size_t n = 0; uint8_t *d = read_file(p, &n);
    if (!d || n < 0x100) { DLOG("[FORK] dex: the save (%s) not readable", p); free(d); return 0; }
    size_t L = n - 0x20; for (size_t i = 0; i < L; i++) d[i] ^= k_pad[i % 0x7F];   // (the last 0x20: a SHA-256, not needed)
    size_t o = 0; int blocks = 0, found = 0;
    while (o + 5 <= L) {
        uint32_t key = rd32(d + o); o += 4; xs32 x = xs_init(key); int t = d[o++] ^ xs_next(&x); size_t len = 0; blocks++;
        if (t >= 1 && t <= 3) continue;
        if (t == 4 || t == 5) { if (o + 4 > L) break; uint8_t c[4]; for (int i = 0; i < 4; i++) c[i] = d[o + i] ^ xs_next(&x); o += 4; uint32_t cnt = rd32(c);
            if (t == 5) { if (o >= L) break; int st = d[o++] ^ xs_next(&x); int sz = type_size(st); if (sz < 0) break; len = (size_t)cnt * sz; } else len = cnt; }
        else { int sz = type_size(t); if (sz < 0) { DLOG("[FORK] dex: save block %#x of unknown type %d: stopped", key, t); break; } len = (size_t)sz; }
        if (o + len > L) break;
        for (int z = 0; z < 3; z++) if (key == k_zkey[z] && t == 4 && len && !g_zsave[z] && (g_zsave[z] = malloc(len))) {
            for (size_t i = 0; i < len; i++) g_zsave[z][i] = d[o + i] ^ xs_next(&x); g_zlen[z] = len; found++; }
        o += len; }
    free(d); DLOG("[FORK] dex: save read: %d blocks, %d Pokédex blocks (%zu / %zu / %zu bytes)", blocks, found, g_zlen[0], g_zlen[1], g_zlen[2]);
    return found > 0;
}
// a Pokédex block in the heap. v57: an early game's Pokédex is nearly all zeros (v56 log: nothing distinctive enough was found), so: the windows of 16 entries
//   with the most non-zero bytes, in each the 8 bytes with the most non-zero ones as the anchor; each place in the heap holding the anchor is checked on the
//   window's non-zero bytes, then on the whole block's (90 % the same: the player may have seen or caught more since the save). Zeros never count as a match
static size_t nz_same(const uint8_t *q, const uint8_t *blk, size_t len, size_t *nz) { size_t same = 0; *nz = 0; for (size_t i = 0; i < len; i++) if (blk[i]) { (*nz)++; same += q[i] == blk[i]; } return same; }
static uint64_t find_block(const uint8_t *blk, size_t len, uint64_t lo, uint64_t hi) {
    uintptr_t b = dm_fork_heap_base(); if (!b || len < 0x300) return 0;
    if (hi > dm_fork_heap_size()) hi = dm_fork_heap_size();
    size_t W = 0x300; int best[3] = { -1, -1, -1 }, bestc[3] = { 0 };
    for (size_t e = 0; e + W <= len; e += 0x30 * 4) { int c = 0; for (size_t i = 0; i < W; i++) c += blk[e + i] != 0;
        for (int k = 0; k < 3; k++) if (c > bestc[k]) { for (int j = 2; j > k; j--) { best[j] = best[j - 1]; bestc[j] = bestc[j - 1]; } best[k] = (int)e; bestc[k] = c; break; } }
    size_t tnz; nz_same(blk, blk, len, &tnz);
    DLOG("[FORK] dex: Pokédex block of %zu bytes, %zu of them not zero; busiest window %d bytes", len, tnz, bestc[0]);
    if (best[0] < 0 || bestc[0] < 3) return 0;
    for (int k = 0; k < 3 && best[k] >= 0 && bestc[k] >= 3; k++) {
        const uint8_t *w = blk + best[k]; size_t ao = 0; int an = -1;
        for (size_t i = 0; i < W; i += 8) { int c = 0; for (int j = 0; j < 8; j++) c += w[i + j] != 0; if (c > an || (c == an && rd64(w + i) > rd64(w + ao))) { an = c; ao = i; } }
        uint64_t v = rd64(w + ao); size_t wnz; nz_same(w, w, W, &wnz); int cands = 0;
        for (uint64_t off = lo + 8; off + 8 <= hi; off += 8) {
            if (*(const uint64_t *)(b + off) != v) continue;
            uint64_t base = off - (uint64_t)best[k] - ao; if (base < lo || base + len > hi || (off - ao) < lo) continue; cands++;
            size_t n1, s1 = nz_same((const uint8_t *)(b + off - ao), w, W, &n1); if (s1 * 10 < n1 * 9) continue;
            size_t n2, s2 = nz_same((const uint8_t *)(b + base), blk, len, &n2); if (s2 * 10 < n2 * 9) continue;
            DLOG("[FORK] dex: Pokédex block matched at heap + %#llx (%zu of %zu non-zero bytes the same; %d places held the anchor)", (unsigned long long)base, s2, n2, cands);
            return base; }
        DLOG("[FORK] dex: window %d: anchor %016llx at %d places, none matched", k, (unsigned long long)v, cands); }
    return 0;
}
static void locate_dex(void) {
    for (int z = 0; z < 3; z++) if (g_zsave[z]) {
        uint64_t t0 = now_ns(), a = find_block(g_zsave[z], g_zlen[z], 0x45000000, 0x45400000);   // (the save blocks SysBot knows are all at 0x4505..-0x450C..)
        if (!a) a = find_block(g_zsave[z], g_zlen[z], 0x1000, 0x45000000); if (!a) a = find_block(g_zsave[z], g_zlen[z], 0x45400000, ~0ull);
        g_zoff[z] = a; DLOG("[FORK] dex: Pokédex block %d %s%#llx (%.1f s)", z, a ? "at heap + " : "NOT found in memory ", (unsigned long long)a, (now_ns() - t0) / 1e9); }
}
// 0 not seen, 1 seen, 2 caught; -1 unknown
static int dex_state(int sp) {
    if (sp <= 0 || sp > 898 || !g_dexidx[sp]) return -1; int z = g_dexblk[sp]; if (!g_zoff[z]) return -1;
    uint64_t e = g_zoff[z] + (uint64_t)(g_dexidx[sp] - 1) * 0x30; if ((uint64_t)(g_dexidx[sp]) * 0x30 > g_zlen[z]) return -1;
    uintptr_t b = dm_fork_heap_base(); if (!b || !dm_guest_heap_readable(b + e, 0x30)) return -1;
    const uint8_t *p = (const uint8_t *)(b + e);
    if (rd32(p + 0x20) & 1) return 2; return rd64(p) || rd64(p + 8) || rd64(p + 16) || rd64(p + 24) ? 1 : 0;
}

// ---------- where the player is ----------
enum { O_MAPHASH = 0x450613C0, NHITS = 16384, NCAND = 16 };
static uint8_t g_zbits[8192];   // zone hashes by their top 16 bits (a quick filter)
static uint64_t g_hit[NHITS], g_hitv[NHITS]; static int g_nhit;
static struct { uint64_t off, val; int score, qn, tick; } g_cand[NCAND]; static int g_ncand; static uint64_t g_lock; static int g_guess = -1, g_ticks;
static int is_zone(uint64_t v) { return v && (g_zbits[v >> 51] >> ((v >> 48) & 7) & 1) && zone_index(v) >= 0; }
static void zone_scan(void) {
    uintptr_t b = dm_fork_heap_base(); uint64_t lim = dm_fork_heap_size(), t0 = now_ns(); int n = 0;
    for (uint64_t off = 0x1000; off + 8 <= lim; off += 8) { uint64_t v = *(const uint64_t *)(b + off); if (!is_zone(v)) continue;
        if (n < NHITS) { g_hit[n] = off; g_hitv[n] = v; n++; } }
    g_nhit = n; DLOG("[FORK] dex probe: %d zone hashes in memory (%.1f s)", n, (now_ns() - t0) / 1e9);
    // v57: a guess while nothing has changed yet. Copies of tables list many zones close together (the spawn tables, maps): 128 KB stretches with 12 different zones
    //   or more are left out; of the rest, the zone found most often is the guess (the loaded area's own data names it). Logged for the next step
    static uint16_t cnt[NZONES]; memset(cnt, 0, sizeof cnt); int lone = 0, said = 0;
    for (int i = 0, j0 = 0; i < n; i++) {
        uint64_t bk = g_hit[i] >> 17; while (j0 < n && (g_hit[j0] >> 17) + 1 < bk) j0++;
        int dz = 0; uint64_t seen[16];
        for (int j = j0; j < n && (g_hit[j] >> 17) <= bk + 1 && dz < 12; j++) { int k = 0; while (k < dz && seen[k] != g_hitv[j]) k++; if (k == dz) seen[dz++] = g_hitv[j]; }
        if (dz >= 12) continue;
        int z = zone_index(g_hitv[i]); if (z < 0) continue; cnt[z]++; lone++;
        if (said < 24 && g_lines < 150) { said++; DLOG("[FORK] dex probe: outside tables: heap + %#llx = %s", (unsigned long long)g_hit[i], k_zones[z].name); } }
    int bz = -1, b2 = 0; for (int z = 0; z < NZONES; z++) { if (bz < 0 || cnt[z] > cnt[bz]) { if (bz >= 0) b2 = cnt[bz]; bz = z; } else if (cnt[z] > b2) b2 = cnt[z]; }
    g_guess = bz >= 0 && cnt[bz] > b2 ? bz : -1;
    DLOG("[FORK] dex probe: %d outside the tables; guess %s (%d, next %d)", lone, g_guess >= 0 ? k_zones[g_guess].name : "none", bz >= 0 ? cnt[bz] : 0, b2);
}
// after a popup: the listed places that now hold another zone hash
// v57: every 5 s (and 2 s after an area popup) the listed places are read again: each that now holds another zone hash is a candidate (changes counted, qn).
//   At a popup, the candidates that changed in the 8 s before it score a point; one that scores twice and does not change much without popups (a counter running
//   through zones would) is kept and read live from then on
static void zone_diff(int popup) {
    uintptr_t b = dm_fork_heap_base(); int ch = 0;
    for (int i = 0; i < g_nhit; i++) { uint64_t v = *(const uint64_t *)(b + g_hit[i]); if (v == g_hitv[i] || !is_zone(v)) continue; ch++;
        int c = 0; while (c < g_ncand && g_cand[c].off != g_hit[i]) c++;
        if (c == g_ncand) { if (g_ncand == NCAND) continue; g_ncand++; g_cand[c].off = g_hit[i]; g_cand[c].score = 0; g_cand[c].qn = 0; }
        g_cand[c].qn++; g_cand[c].val = v; g_cand[c].tick = g_ticks;
        if (g_cand[c].qn <= 3) DLOG("[FORK] dex probe: heap + %#llx changed %016llx -> %016llx (%s), change %d", (unsigned long long)g_hit[i], (unsigned long long)g_hitv[i], (unsigned long long)v, k_zones[zone_index(v)].name, g_cand[c].qn);
        g_hitv[i] = v; }
    if (popup) { int m = 0;
        for (int c = 0; c < g_ncand; c++) if (g_ticks - g_cand[c].tick <= 16) { g_cand[c].score++; m++;
            DLOG("[FORK] dex probe: area popup %d: heap + %#llx changed with it (%s; %d popups, %d changes)", atomic_load(&dm_fork_area_popups), (unsigned long long)g_cand[c].off, k_zones[zone_index(g_cand[c].val)].name, g_cand[c].score, g_cand[c].qn); }
        if (!m) DLOG("[FORK] dex probe: area popup %d: no listed place changed", atomic_load(&dm_fork_area_popups)); }
    if (!g_lock) for (int c = 0; c < g_ncand; c++) if (g_cand[c].score >= 2 && g_cand[c].qn <= g_cand[c].score + 2) { int tie = 0;
        for (int d = 0; d < g_ncand; d++) tie += d != c && g_cand[d].score >= g_cand[c].score && g_cand[d].qn <= g_cand[d].score + 2;
        if (!tie) { g_lock = g_cand[c].off; DLOG("[FORK] dex probe: the area is at heap + %#llx (it changed with each area popup)", (unsigned long long)g_lock); } }
}
static int current_zone(const char **how) {
    uintptr_t b = dm_fork_heap_base(); if (!b) return -1;
    if (g_lock) { int z = zone_index(*(const uint64_t *)(b + g_lock)); if (z >= 0) { *how = "live"; return z; } }
    int best = -1; for (int c = 0; c < g_ncand; c++) if (g_cand[c].score >= 1 && g_cand[c].qn <= g_cand[c].score + 2 && (best < 0 || g_cand[c].tick > g_cand[best].tick)) best = c;
    if (best >= 0) { int z = zone_index(*(const uint64_t *)(b + g_cand[best].off)); if (z >= 0) { *how = "probe"; return z; } }
    if (dm_guest_heap_readable(b + O_MAPHASH, 8)) { int z = zone_index(*(const uint64_t *)(b + O_MAPHASH)); if (z >= 0) { *how = "saved"; return z; } }
    if (g_guess >= 0) { *how = "guess"; return g_guess; }   // v57
    return -1;
}

// ---------- what the panel shows ----------
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static dm_fork_dex_info g_info = { .active = 1, .area = "AREA DEX", .msg = { "STARTING (1 MIN)", "" } };
// v56: the panel always shows (Steven, Nov 19: v55 showed nothing at all), with what it waits for
static void status(const char *a, const char *b) { pthread_mutex_lock(&g_mu); memset(&g_info, 0, sizeof g_info); g_info.active = 1; snprintf(g_info.area, sizeof g_info.area, "AREA DEX");
    snprintf(g_info.msg[0], sizeof g_info.msg[0], "%s", a); snprintf(g_info.msg[1], sizeof g_info.msg[1], "%s", b); pthread_mutex_unlock(&g_mu); }
static _Atomic int g_wnext;   // touch_ui.c: taps on the weather line
int dm_fork_dex_get(dm_fork_dex_info *o) {
    static _Atomic int started; void dm_fork_dex_start(void); if (!atomic_exchange(&started, 1)) dm_fork_dex_start();
    pthread_mutex_lock(&g_mu); *o = g_info; pthread_mutex_unlock(&g_mu); return o->active;
}
void dm_fork_dex_weather_next(void) { atomic_fetch_add(&g_wnext, 1); }
static int base_len(const char *s) { const char *p = strstr(s, " ("); return p ? (int)(p - s) : (int)strlen(s); }
static int same_place(int a, int b) { int la = base_len(k_zones[a].name), lb = base_len(k_zones[b].name); return la == lb && !strncmp(k_zones[a].name, k_zones[b].name, (size_t)la); }
static void build(int z, int *wsel, int wtaps, int guessed) {
    dm_fork_dex_info e = { 0 }; e.active = 1; e.weather = -1;
    snprintf(e.area, sizeof e.area, "%.*s%s", base_len(k_zones[z].name), k_zones[z].name, guessed ? "?" : "");   // (v57: "?": a guess)
    unsigned mask = 0; for (int i = 0; i < g_nslots; i++) if (same_place(g_slots[i].zone, z)) mask |= 1u << g_slots[i].sub;
    // the weather / kind shown: all (-1), then each one the place has, a tap at a time
    int opts[12], no = 0; opts[no++] = -1; for (int s = 0; s < 11; s++) if (mask >> s & 1) opts[no++] = s;
    *wsel = ((*wsel + wtaps) % no + no) % no; int w = opts[*wsel]; e.weather = w;
    int dexok = 0;
    for (int i = 0; i < g_nslots; i++) { const slot *s = &g_slots[i]; if (!same_place(s->zone, z) || (w >= 0 && s->sub != w)) continue;
        int r = 0; while (r < e.n && (e.row[r].species != s->species || e.row[r].form != s->form)) r++;
        if (r == e.n) { if (e.n == 40) continue; e.n++; e.row[r] = (dm_fork_dex_row){ (int16_t)s->species, s->form, 0, 0, 0, 0 };
            int st = dex_state(s->species); if (st >= 0) dexok = 1; e.row[r].state = (uint8_t)(st < 0 ? 1 : st); }
        if (s->prob > e.row[r].pct) e.row[r].pct = s->prob; e.row[r].flags |= s->kind ? 2 : 1; }
    e.dexok = dexok;
    for (int i = 1; i < e.n; i++) for (int j = i; j > 0 && e.row[j].pct > e.row[j - 1].pct; j--) { dm_fork_dex_row t = e.row[j]; e.row[j] = e.row[j - 1]; e.row[j - 1] = t; }
    pthread_mutex_lock(&g_mu); g_info = e; pthread_mutex_unlock(&g_mu);
}
static void *worker(void *arg) {
    (void)arg; struct timespec ts = { 0, 500000000 };
    while (!dm_fork_heap_base() || !g_nmod) nanosleep(&ts, NULL);
    for (int i = 0; i < 90; i++) nanosleep(&ts, NULL);   // (45 s: the save is loaded and the player in the field)
    if (!is_sword()) { DLOG("[FORK] dex: not Sword 1.3.2: off"); status("SWORD 1.3.2 ONLY", ""); return NULL; }
    status("READING THE GAME", "");
    for (int i = 0; i < NZONES; i++) g_zbits[k_zones[i].hash >> 51] |= (uint8_t)(1 << ((k_zones[i].hash >> 48) & 7));
    if (!load_tables()) { status("NO SPAWN DATA", "(SEE THE LOG)"); return NULL; }
    load_personal(); if (load_save()) locate_dex();
    { uintptr_t b = dm_fork_heap_base(); uint64_t m = dm_guest_heap_readable(b + O_MAPHASH, 8) ? *(const uint64_t *)(b + O_MAPHASH) : 0; int z = zone_index(m);
      DLOG("[FORK] dex: started, %d spawn slots", g_nslots); DLOG("[FORK] dex: saved map hash %016llx: %s", (unsigned long long)m, z >= 0 ? k_zones[z].name : "not a zone"); }
    zone_scan();
    int seen = &dm_fork_area_popups ? atomic_load(&dm_fork_area_popups) : 0, due = 0, lastz = -2, wsel = 0, taps = 0;
    for (;;) {
        nanosleep(&ts, NULL);
        if (&dm_fork_dex_on && !dm_fork_dex_on) continue;
        int pc = &dm_fork_area_popups ? atomic_load(&dm_fork_area_popups) : 0;
        if (pc != seen) { seen = pc; due = 4; }   // (2 s after the popup: the area has been set by then)
        g_ticks++;
        if (due && !--due) { zone_diff(1); if (!g_lock) zone_scan(); }
        else if (!g_lock && g_ticks % 10 == 0) zone_diff(0);   // v57: every 5 s too
        if (!g_lock && g_ticks % 120 == 0) zone_scan();   // (and a fresh list every minute)
        const char *how = ""; int z = current_zone(&how), t = atomic_exchange(&g_wnext, 0);
        if (z != lastz) { wsel = 0; DLOG("[FORK] dex: area %s (%s)", z >= 0 ? k_zones[z].name : "unknown", z >= 0 ? how : "-"); lastz = z; }
        if (z < 0) { status("AREA NOT KNOWN YET", "WALK TO A NEW AREA"); continue; }
        taps += t; build(z, &wsel, taps, !strcmp(how, "guess")); taps = 0;
    }
    return NULL;
}
void dm_fork_dex_start(void) { pthread_t t; if (!pthread_create(&t, NULL, worker, NULL)) pthread_detach(t); }
