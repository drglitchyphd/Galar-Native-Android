// FORK v47: encounter info, step 1: a log-only probe of where Sword 1.3.2 keeps its Pokémon (docs/ENCOUNTER_INFO_RESEARCH.md). Nothing is drawn or written.
// The offsets (SysBot.NET, CaptureSight, PyNXReader, all for main build a3b75bcd...) are heap base + offset; our host's heap base is the one the game gets from
// SetHeapSize / GetInfo(4) (hle_svc.c g_heap: dm_fork_heap_base). Every read goes through dm_guest_heap_readable. PK8 decryption after PKHeX (PokeCrypto.Decrypt8).
// Logged ([FORK] enc ...): once after the game has run a while, the heap base, the trainer (name, TID / SID), party slot 1 (species, checksum); then each new wild
// Pokémon while the in-battle flag is set (species, shiny, IVs, nature, ability, EVs, checksum). The checksum and a matching trainer name tell whether the offsets
// hold on this host. Called from gfx/nvn/hle_nvn.c next to the other fork ticks (every present; it looks every 30th).
#include "hle.h"
#include <stdatomic.h>
size_t dm_fork_heap_size(void);   // hle_svc.c (FORK)

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
// v48: what the encounter panel shows (android/app/jni/touch_ui.c dm_fork_enc_get): the wild Pokémon of this battle. A battle counts as wild when the wild slot
//   holds a new Pokémon (another encryption constant than the last battle's): in a trainer battle it still holds the last wild one, so nothing is shown
typedef struct { int active, species, shiny, nature, ability, iv[6], ev[6]; } dm_fork_enc_info;   // iv / ev: HP, Atk, Def, SpA, SpD, Spe
static pthread_mutex_t g_enc_mu = PTHREAD_MUTEX_INITIALIZER; static dm_fork_enc_info g_enc;
int dm_fork_enc_get(dm_fork_enc_info *o) { pthread_mutex_lock(&g_enc_mu); *o = g_enc; pthread_mutex_unlock(&g_enc_mu); return o->active; }
static void enc_set(const uint8_t *d, int active) {
    dm_fork_enc_info e = { 0 };
    if (active) { uint32_t pid = rd32(d + 0x1C), x = rd32(d + 0x0C) ^ pid, xr = (x ^ (x >> 16)) & 0xFFFF, iv = rd32(d + 0x8C);
        e.active = 1; e.species = rd16(d + 8); e.shiny = xr < 16 ? (xr == 0 ? 2 : 1) : 0; e.nature = d[0x20] < 25 ? d[0x20] : 0; e.ability = rd16(d + 0x14);
        int sh[6] = { 0, 5, 10, 20, 25, 15 }; for (int i = 0; i < 6; i++) e.iv[i] = (iv >> sh[i]) & 31;
        int eo[6] = { 0x26, 0x27, 0x28, 0x2A, 0x2B, 0x29 }; for (int i = 0; i < 6; i++) e.ev[i] = d[eo[i]]; }
    pthread_mutex_lock(&g_enc_mu); g_enc = e; pthread_mutex_unlock(&g_enc_mu);
}
// v51: SysBot's wild offset does not hold on this host (Steven's v48 log: a failed checksum there), while the trainer and party offsets do. So the wild Pokémon is
//   looked for: every 8 bytes of the heap whose header looks like a PK8 (sanity word 0, encryption constant and checksum not 0) gets its species and trainer ID
//   decrypted (the stream jumped to their block, lcg_jump), and only one with a species 1-898 and the PLAYER's TID / SID (a wild Pokémon carries them, a trainer's
//   does not) is decrypted in full and checksum-checked. The party and box block is skipped. A find is remembered (g_wild_off) and read first next time
static _Atomic uint64_t g_wild_off; static _Atomic int g_scanning; static int g_battle_found, g_scan_tried; static uint64_t g_battle_t;
static uint16_t g_scan_tid, g_scan_sid; static uint32_t g_scan_skip_ec;
static uint32_t g_jA[130], g_jC[130];   // the LCG stepped n times: seed_n = g_jA[n] * seed + g_jC[n]
static void lcg_tables(void) { uint32_t a = 1, c = 0; for (int i = 0; i < 130; i++) { g_jA[i] = a; g_jC[i] = c; a *= 0x41C64E6Du; c = c * 0x41C64E6Du + 0x6073u; } }
static int lines_g;
#define LOG_LIMIT(...) do { if (lines_g < 60) { lines_g++; LOG(__VA_ARGS__); } } while (0)
static int trainer_ids(uint16_t *tid, uint16_t *sid) { uint8_t ms[0xA4]; if (!read_heap(O_MYSTATUS, ms, sizeof ms)) return 0; *tid = rd16(ms + 0xA0); *sid = rd16(ms + 0xA2); return *tid || *sid; }
static void *find_wild(void *arg) {
    (void)arg; uintptr_t b = dm_fork_heap_base(); uint64_t t0 = now_ns(), hits = 0, best = 0; if (!g_jA[1]) lcg_tables();
    uint64_t lim = dm_fork_heap_size(); if (lim > 0x148) lim -= 0x148; else lim = 0;
    for (uint64_t off = 0x1000; off < lim; off += 8) {
        if (off >= 0x45075000 && off < 0x450CB000) { off = 0x450CB000 - 8; continue; }   // (the boxes and the party)
        const uint8_t *p = (const uint8_t *)(b + off);
        if (p[4] | p[5]) continue;
        uint32_t ec = rd32(p); if (!ec || !rd16(p + 6)) continue;
        int pa = k_block[(ec >> 13) & 31][0]; uint32_t wi = (uint32_t)pa * 40;   // block A (species, TID, SID) sits at encrypted block pa
        uint32_t s1 = g_jA[wi + 1] * ec + g_jC[wi + 1]; const uint8_t *q = p + 8 + pa * 0x50;
        uint16_t sp = rd16(q) ^ (uint16_t)(s1 >> 16); if (!sp || sp > 898) continue;
        uint32_t s3 = g_jA[wi + 3] * ec + g_jC[wi + 3]; if ((uint16_t)(rd16(q + 4) ^ (uint16_t)(s3 >> 16)) != g_scan_tid) continue;
        uint32_t s4 = g_jA[wi + 4] * ec + g_jC[wi + 4]; if ((uint16_t)(rd16(q + 6) ^ (uint16_t)(s4 >> 16)) != g_scan_sid) continue;
        uint8_t d[0x148]; if (read_pk8(off, d) <= 0) continue;
        hits++; LOG_LIMIT("[FORK] enc search: a Pokémon with your ID at heap + %#llx: species %u", (unsigned long long)off, rd16(d + 8));
        if (!best && rd32(d) != g_scan_skip_ec) best = off;
        if (hits >= 12) break;
    }
    LOG_LIMIT("[FORK] enc search: %llu found in %.1f s%s", (unsigned long long)hits, (now_ns() - t0) / 1e9, best ? "" : " (none new: a trainer battle, or not in memory yet)");
    if (best) atomic_store(&g_wild_off, best);
    atomic_store(&g_scanning, 0); return NULL;
}
void dm_fork_enc_tick(void) {
    static uint64_t n; static int said, lines, was_in; static uint32_t last_ec, battle_ec; (void)last_ec;
    if (++n % 15 || !is_sword()) return;
    { static int on = -1; extern volatile int dm_fork_enc_on __attribute__((weak));   // v49: only while Encounter info is on (touch_ui.c sets it; before that, env.txt's DM_FORK_ENC)
      if (on < 0) { const char *e = getenv("DM_FORK_ENC"); on = e && !strcmp(e, "on"); }
      if (!(&dm_fork_enc_on ? dm_fork_enc_on : on)) return; }
    if (!said && n >= 30 * 60 && lines < 60) {   // after about a minute: the save is loaded
        said = 1; uint8_t ms[0x110]; LOG("[FORK] enc: heap base %#llx", (unsigned long long)dm_fork_heap_base());
        if (read_heap(O_MYSTATUS, ms, sizeof ms)) {
            char name[13] = { 0 }; for (int i = 0; i < 12; i++) { uint16_t c = rd16(ms + 0xB0 + 2 * i); if (!c) break; name[i] = c < 128 ? (char)c : '?'; }
            LOG("[FORK] enc trainer: name \"%s\", TID %u, SID %u (should match your save: if not, the heap offsets do not hold here)", name, rd16(ms + 0xA0), rd16(ms + 0xA2)); }
        else LOG("[FORK] enc trainer: not readable at heap + %#x", O_MYSTATUS);
        uint8_t p[0x148]; int ok = read_pk8(O_PARTY, p); if (ok >= 0) log_pk8("party slot 1", p, ok); else LOG("[FORK] enc party: not readable"); lines++;
    }
    // v51: in a battle by the game's flag, or by the fork's screen guess (gfx/nvn/fork_ui.h dm_fork_screen 2)
    extern _Atomic int dm_fork_screen __attribute__((weak));
    uint8_t fl; int in = (read_heap(O_INBATTLE, &fl, 1) && fl) || (&dm_fork_screen && atomic_load(&dm_fork_screen) == 2);
    if (!in) { if (was_in) { enc_set(NULL, 0); LOG_LIMIT("[FORK] enc: battle over"); } was_in = 0; g_battle_found = 0; return; }
    if (!was_in) { was_in = 1; g_battle_found = 0; g_scan_tried = 0; g_battle_t = n; }
    if (g_battle_found) return;
    // the wild Pokémon: the last place it was found (or SysBot's offset), then a search of the heap (find_wild, on its own thread)
    uint16_t tid, sid; if (!trainer_ids(&tid, &sid)) return;
    uint64_t offs[2] = { atomic_load(&g_wild_off), O_WILD };
    int old = 0;
    for (int k = 0; k < 2; k++) { uint8_t w[0x148]; if (!offs[k] || read_pk8(offs[k], w) <= 0) continue;
        uint32_t ec = rd32(w); if (rd16(w + 0x0C) != tid || rd16(w + 0x0E) != sid) continue;
        if (ec == battle_ec) { old = 1; continue; }   // (the last battle's wild Pokémon still there: a trainer battle, or not written yet)
        battle_ec = ec; g_battle_found = 1; atomic_store(&g_wild_off, offs[k]); enc_set(w, 1);
        if (lines < 60) { lines++; LOG("[FORK] enc: the wild Pokémon at heap + %#llx", (unsigned long long)offs[k]); log_pk8("wild", w, 1); }
        return; }
    if (old && n - g_battle_t < 150) return;   // (the known place still holds the last one: wait up to 5 s for a new one before searching)
    if (old) { if (!g_scan_tried) { g_scan_tried = 1; LOG_LIMIT("[FORK] enc: the wild slot still holds the last battle's Pokémon: a trainer battle (no panel)"); } return; }
    if (!g_scan_tried && !atomic_load(&g_scanning) && n - g_battle_t >= 60) {   // (after 2 s in the battle: the wild Pokémon has been written by then)
        g_scan_tried = 1; atomic_store(&g_scanning, 1); g_scan_tid = tid; g_scan_sid = sid; g_scan_skip_ec = battle_ec;
        pthread_t t; if (pthread_create(&t, NULL, find_wild, NULL)) atomic_store(&g_scanning, 0); else pthread_detach(t); }
    { static int said_trainer; if (g_scan_tried && !atomic_load(&g_scanning) && !g_battle_found && said_trainer != (int)g_battle_t) { said_trainer = (int)g_battle_t;
        if (lines < 60) { lines++; LOG("[FORK] enc: no wild Pokémon with your trainer ID in memory: a trainer battle (no panel)"); } } }
}
