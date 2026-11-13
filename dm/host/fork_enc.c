// FORK v47: encounter info, step 1: a log-only probe of where Sword 1.3.2 keeps its Pokémon (docs/ENCOUNTER_INFO_RESEARCH.md). Nothing is drawn or written.
// The offsets (SysBot.NET, CaptureSight, PyNXReader, all for main build a3b75bcd...) are heap base + offset; our host's heap base is the one the game gets from
// SetHeapSize / GetInfo(4) (hle_svc.c g_heap: dm_fork_heap_base). Every read goes through dm_guest_heap_readable. PK8 decryption after PKHeX (PokeCrypto.Decrypt8).
// Logged ([FORK] enc ...): once after the game has run a while, the heap base, the trainer (name, TID / SID), party slot 1 (species, checksum); then each new wild
// Pokémon while the in-battle flag is set (species, shiny, IVs, nature, ability, EVs, checksum). The checksum and a matching trainer name tell whether the offsets
// hold on this host. Called from gfx/nvn/hle_nvn.c next to the other fork ticks (every present; it looks every 30th).
#include "hle.h"

uintptr_t dm_fork_heap_base(void);                  // hle_svc.c (FORK)
int dm_guest_heap_readable(uint64_t a, size_t n);   // hle_svc.c

enum { O_WILD = 0x8FEA3648, O_PARTY = 0x450C68B0, O_MYSTATUS = 0x45068F18, O_INBATTLE = 0x3F128624 };
static const uint8_t k_block[32][4] = {
    {0,1,2,3},{0,1,3,2},{0,2,1,3},{0,3,1,2},{0,2,3,1},{0,3,2,1},{1,0,2,3},{1,0,3,2},{2,0,1,3},{3,0,1,2},{2,0,3,1},{3,0,2,1},
    {1,2,0,3},{1,3,0,2},{2,1,0,3},{3,1,0,2},{2,3,0,1},{3,2,0,1},{1,2,3,0},{1,3,2,0},{2,1,3,0},{3,1,2,0},{2,3,1,0},{3,2,1,0},
    {0,1,2,3},{0,1,3,2},{0,2,1,3},{0,3,1,2},{0,2,3,1},{0,3,2,1},{1,0,2,3},{1,0,3,2} };
static const char *const k_nature[25] = { "Hardy","Lonely","Brave","Adamant","Naughty","Bold","Docile","Relaxed","Impish","Lax","Timid","Hasty","Serious","Jolly",
    "Naive","Modest","Mild","Quiet","Bashful","Rash","Calm","Gentle","Sassy","Careful","Quirky" };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static void crypt(uint8_t *p, size_t len, uint32_t seed) { for (size_t i = 0; i + 1 < len; i += 2) { seed = seed * 0x41C64E6Du + 0x6073u; uint16_t k = (uint16_t)(seed >> 16); p[i] ^= (uint8_t)k; p[i + 1] ^= (uint8_t)(k >> 8); } }
static int read_heap(uint64_t off, void *dst, size_t n) {
    uintptr_t b = dm_fork_heap_base(); if (!b) return 0;
    uint64_t a = (uint64_t)b + off; if (!dm_guest_heap_readable(a, n)) return 0;
    memcpy(dst, (const void *)(uintptr_t)a, n); return 1;
}
// a 0x148 PK8 at heap + off, decrypted when encrypted (PKHeX: nickname / OT terminators non-zero), checksum checked: 1 valid, 0 not, -1 unreadable
static int read_pk8(uint64_t off, uint8_t d[0x148]) {
    if (!read_heap(off, d, 0x148)) return -1;
    if (rd16(d + 0x70) || rd16(d + 0x110)) {
        uint32_t ec = rd32(d), sv = (ec >> 13) & 31; crypt(d + 8, 0x140, ec);
        uint8_t t[0x140]; memcpy(t, d + 8, 0x140); for (int i = 0; i < 4; i++) memcpy(d + 8 + i * 0x50, t + k_block[sv][i] * 0x50, 0x50); }
    uint16_t sum = 0; for (int i = 8; i < 0x148; i += 2) sum = (uint16_t)(sum + rd16(d + i));
    return sum == rd16(d + 6) && rd16(d + 8) != 0;
}
static void log_pk8(const char *what, const uint8_t *d, int ok) {
    uint32_t pid = rd32(d + 0x1C), id = rd32(d + 0x0C), x = id ^ pid, xr = (x ^ (x >> 16)) & 0xFFFF, iv = rd32(d + 0x8C);
    int nat = d[0x20] < 25 ? d[0x20] : 0;
    LOG("[FORK] enc %s: species %u, %s, IVs %u/%u/%u/%u/%u/%u (HP/Atk/Def/SpA/SpD/Spe), nature %s, ability %u (#%u), EVs %u/%u/%u/%u/%u/%u, checksum %s",
        what, rd16(d + 8), xr == 0 ? "SHINY (square)" : xr < 16 ? "SHINY (star)" : "not shiny",
        iv & 31, (iv >> 5) & 31, (iv >> 10) & 31, (iv >> 20) & 31, (iv >> 25) & 31, (iv >> 15) & 31, k_nature[nat], rd16(d + 0x14), d[0x16] & 7,
        d[0x26], d[0x27], d[0x28], d[0x2A], d[0x2B], d[0x29], ok > 0 ? "ok" : "BAD (offset wrong here?)");
}
static int is_sword(void) {
    static int v = -1; if (v >= 0) return v;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main") && g_mod[i].build_id[0]) { v = !strcasecmp(g_mod[i].build_id, "a3b75bcd3311385aeed67fbeeb79cbb7"); return v; }
    return 0;
}
void dm_fork_enc_tick(void) {
    static uint64_t n; static int said, lines; static uint32_t last_ec;
    if (++n % 30 || !is_sword() || lines > 60) return;
    if (!said && n >= 30 * 60) {   // after about a minute: the save is loaded
        said = 1; uint8_t ms[0x110]; LOG("[FORK] enc: heap base %#llx", (unsigned long long)dm_fork_heap_base());
        if (read_heap(O_MYSTATUS, ms, sizeof ms)) {
            char name[13] = { 0 }; for (int i = 0; i < 12; i++) { uint16_t c = rd16(ms + 0xB0 + 2 * i); if (!c) break; name[i] = c < 128 ? (char)c : '?'; }
            LOG("[FORK] enc trainer: name \"%s\", TID %u, SID %u (should match your save: if not, the heap offsets do not hold here)", name, rd16(ms + 0xA0), rd16(ms + 0xA2)); }
        else LOG("[FORK] enc trainer: not readable at heap + %#x", O_MYSTATUS);
        uint8_t p[0x148]; int ok = read_pk8(O_PARTY, p); if (ok >= 0) log_pk8("party slot 1", p, ok); else LOG("[FORK] enc party: not readable"); lines++;
    }
    uint8_t fl; if (!read_heap(O_INBATTLE, &fl, 1) || !fl) return;
    uint8_t w[0x148]; int ok = read_pk8(O_WILD, w); if (ok < 0) return;
    uint32_t ec = rd32(w); if (ec == last_ec) return; last_ec = ec;
    log_pk8("wild", w, ok); lines++;
}
