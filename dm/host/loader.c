// Multi-module loading (playing the role of Horizon's rtld): mapping, claiming thread slots, gate registration, cross-module relocation, take-over by name, segment permissions.
// The code area (original text and patch area) is set to read-only+execute as soon as it is copied in, and from then on the loader only writes data: on iOS these two blocks must be embedded as is as signed code segments.
#include "dm.h"
#include <TargetConditionals.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include "hle.h"

module_t g_mod[MAX_MOD];
int g_nmod;
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
enum { KEY_K18, KEY_KTLS, KEY_KTLR, KEY_KGATE, KEY_N };

uint64_t resolve_import(module_t *m, const char *name);
int hle_implements(const char *name);
void gate_register(uint64_t ret_addr, const char *kind, int arg, uint64_t orig);

static pthread_key_t key_value(unsigned i) {
    switch (i) {
    case KEY_K18: return g_key18; case KEY_KTLS: return g_keytls; case KEY_KTLR: return g_keytlr;
    default: return g_keygate;
    }
}
static void create_keys(void) {
    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||
        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) die("cannot create the host key");
    LOG("key numbers K18=%lu KTLS=%lu KTLR=%lu KGATE=%lu", g_key18, g_keytls, g_keytlr, g_keygate);
}

#ifdef __ANDROID__
// Android: the slot block of this thread, published in bionic's application TLS slot (TLS_SLOT_APP = 2, tpidr_el0 + 16; bionic leaves it to the app) on first use.
// Entry 0 stays unused so that no key page entry is 0 (install_key_page's check)
static __thread uint64_t g_slot_blk[1 + KEY_N];
static uint64_t *slot_block(void) {
    uint64_t *tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    if (tp[2] != (uint64_t)(uintptr_t)g_slot_blk) {
        if (tp[2]) die("bionic's application TLS slot is already in use (%#llx)", (unsigned long long)tp[2]);
        tp[2] = (uint64_t)(uintptr_t)g_slot_blk;
    }
    return g_slot_blk;
}
void dm_slot_set(int slot, void *v) { slot_block()[1 + slot] = (uint64_t)(uintptr_t)v; }
static void tsd_check(void) {   // the key page entry of K18 must lead patched code to this thread's K18 slot
    dm_slot_set(DM_SLOT_K18, (void *)0x1234);
    uint64_t *tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);
    if (((uint64_t *)(uintptr_t)tp[2])[off / 8] != 0x1234) die("thread slot layout mismatch");
    dm_slot_set(DM_SLOT_K18, 0);
}
#else
void dm_slot_set(int slot, void *v) { pthread_setspecific(key_value((unsigned)slot), v); }
// K slot check: the pthread TSD layout is fixed, slot = tpidrro_el0 + 8*key
static void tsd_check(void) {
    pthread_setspecific(g_key18, (void *)0x1234);
    uint64_t base; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);
    if (((uint64_t *)(base & ~7ull))[off / 8] != 0x1234) die("TSD layout mismatch");
    pthread_setspecific(g_key18, 0);
}
#endif

static void build_symhash(module_t *m);
static void parse_dynamic(module_t *m) {
    uint32_t mod0 = *(uint32_t *)(m->base + 4);
    if (memcmp(m->base + mod0, "MOD0", 4)) die("%s: no MOD0", m->name);
    m->dyn = (const Elf64_Dyn *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf64_Sym *)(m->base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(m->base + d->d_un.d_ptr); break;
        case DT_INIT: m->init = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: m->init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->d_un.d_val; break;
    }
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym));
    build_symhash(m);
}

static void *module_sym_linear(module_t *m, const char *name) {
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i];
        if (s->st_shndx && !strcmp(m->strtab + s->st_name, name)) return m->base + s->st_value;
    }
    return NULL;
}
// Symbol name hash table: inserted in ascending index order, not overwritten if the same name is already there ⇒ lookup results are identical, entry by entry, to a linear scan (the first defined symbol with that name).
// Of the 91–99 ms of ① host loading, relocation + forced take-over took 77–86 ms, mainly because every by-name relocation scanned the symbol tables of two modules linearly
static uint32_t sym_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static void build_symhash(module_t *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->symh = calloc(cap, sizeof *m->symh); m->symh_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i]; if (!s->st_shndx) continue;
        const char *nm = m->strtab + s->st_name;
        for (unsigned j = sym_hash(nm) & m->symh_mask;; j = (j + 1) & m->symh_mask) {
            if (!m->symh[j]) { m->symh[j] = i; break; }
            if (!strcmp(m->strtab + m->symtab[m->symh[j]].st_name, nm)) break;   // The same name already exists with a smaller index: keep that one
        }
    }
}
static unsigned long g_symchk_n, g_symchk_bad;
void *module_sym(module_t *m, const char *name) {
    unsigned long nq; (void)nq;
    if (!m->symh) return module_sym_linear(m, name);
    void *r = NULL;
    for (unsigned j = sym_hash(name) & m->symh_mask; m->symh[j]; j = (j + 1) & m->symh_mask) {
        const Elf64_Sym *s = &m->symtab[m->symh[j]];
        if (!strcmp(m->strtab + s->st_name, name)) { r = m->base + s->st_value; break; }
    }
    static int chk = -1; if (chk < 0) chk = getenv("DM_SYMCHECK") != NULL;
    if (chk) { void *l = module_sym_linear(m, name); g_symchk_n++; nq = g_symchk_n;
               if (l != r) { g_symchk_bad++; LOG("[RED] DM_SYMCHECK: %s in %s hash %p ≠ linear %p", name, m->name, r, l); } }
    return r;
}
void *guest_sym(const char *name) {
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return p; }
    return NULL;
}
// For modules with the patch area before the base (patch_off < 0) addresses in the patch area count too, as for modules whose patch area follows the image (crash reports, the GC pause's "in guest code" test)
module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++) {
        const uint8_t *b = g_mod[i].base;
        if ((const uint8_t *)p >= b && (const uint8_t *)p < b + g_mod[i].size) return &g_mod[i];
        if (g_mod[i].patch_off < 0 && (const uint8_t *)p >= b + g_mod[i].patch_off && (const uint8_t *)p < b + g_mod[i].patch_off + g_mod[i].patch_len) return &g_mod[i];
    }
    return NULL;
}
// For callers without dm.h (android_main.c's load profile, fable-loads.md I3 / I4): an address as "<module>+0x<offset>" (1) or not in a game module (0), and
//   module i's name and build id (0 past the last)
int dm_addr_module(const void *p, char *out, size_t n) {
    module_t *m = module_of(p); if (!m) return 0;
    snprintf(out, n, "%s+0x%llx", m->name, (unsigned long long)((const uint8_t *)p - m->base)); return 1;
}
int dm_module_info(int i, const char **name, const char **build) { if (i < 0 || i >= g_nmod) return 0; *name = g_mod[i].name; *build = g_mod[i].build_id; return 1; }

// SDK major version, from the version string the SDK's libraries keep in the sdk module's read-only data ("NintendoSDK_nnSdk-10_4_0"; SDK 12 spells it NintendoSdk_).
// Behaviour that follows the SDK version is decided by it (the NVN API version, the shared-font layout), not by the game profile. 0 = no sdk module or no version string
int dm_sdk_major(void) {
    static int major = -1;
    if (major >= 0) return major;
    major = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) {
        const char *b = (const char *)g_mod[i].base + g_mod[i].ro_off, *e = (const char *)g_mod[i].base + g_mod[i].key_page_off;   // read-only data, data and bss
        for (const char *p = b; (p = memmem(p, (size_t)(e - p), "_nnSdk-", 7)); p += 7)
            if (p - b >= 11 && !strncasecmp(p - 11, "NintendoSDK", 11) && p[7] >= '0' && p[7] <= '9') {
                major = atoi(p + 7); LOG("sdk: NintendoSDK nnSdk-%.12s (major %d)", p + 7, major); break; }
    }
    if (!major) LOG("[RED] sdk: no SDK version string in the sdk module; SDK-dependent behaviour takes the newer SDKs' side");
    return major;
}

// Cross-module symbol resolution: first look for a definition in the loaded modules, and only if not found hand over to HLE / stubs
static uint64_t resolve_sym(module_t *m, const char *name) {
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return (uint64_t)p; }
    return resolve_import(m, name);
}

// Forced take-over table (hle.h HLE_FORCE, the __DATA,swforce section): a hit overrides the definition inside the module
static const hle_force_ent *g_force; static unsigned g_nforce; static unsigned *g_force_hits;
static void force_init(void) {
    unsigned long sz; g_force = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &sz);
    g_nforce = g_force ? (unsigned)(sz / sizeof *g_force) : 0;
    g_force_hits = calloc(g_nforce + 1, sizeof *g_force_hits);
    for (unsigned i = 0; i < g_nforce; i++)
        for (unsigned j = i + 1; j < g_nforce; j++)
            if (!strcmp(g_force[i].name, g_force[j].name)) die("duplicate entry in the forced takeover list: %s", g_force[i].name);
}
// "soft crypto" takeovers (hle_softcrypto.c) only on a CPU without the ARMv8 Cryptographic Extension (the Android emulator's ARM translation); DM_SOFT_CRYPTO=1 forces them
#if defined(__linux__) && defined(__aarch64__)
#include <sys/auxv.h>
static int soft_crypto(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_SOFT_CRYPTO"); unsigned long hw = getauxval(AT_HWCAP), need = (1ul << 5) | (1ul << 6) | (1ul << 3);   // HWCAP_SHA1 | HWCAP_SHA2 | HWCAP_AES
                  on = e ? atoi(e) != 0 : (hw & need) != need; if (on) LOG("soft crypto: the CPU lacks SHA1 / SHA2 / AES instructions (hwcap %#lx): nn::crypto's SHA blocks run in C", hw); }
    return on;
}
#else
static int soft_crypto(void) { const char *e = getenv("DM_SOFT_CRYPTO"); return e && atoi(e); }
#endif
static void *force_lookup(const char *name) {
    for (unsigned i = 0; i < g_nforce; i++)
        if (!strcmp(g_force[i].name, name)) {
            if (!strncmp(g_force[i].why, "soft crypto", 11) && !soft_crypto()) return NULL;   // (the SDK's own hardware code)
            if (!g_force_hits[i]++ && g_flag.imports) LOG("force %s  (%s)", name, g_force[i].why);
            void *hlecount_wrap(const char *, const char *, void *); return hlecount_wrap("FORCE", name, g_force[i].fn);   // A counting stub when DM_HLECOUNT is set
        }
    return NULL;
}
// Generic host: the list serves several games, so entries a game does not reference are only noted; with DM_FORCE_STRICT=1 it still fails loudly (for checking a mistyped list)
static void force_check(void) {
    unsigned unused = 0; int strict = getenv("DM_FORCE_STRICT") != NULL;
    for (unsigned i = 0; i < g_nforce; i++)
        if (!g_force_hits[i]) { if (strict || g_flag.imports) LOG("forced takeover entry with no reference at all: %s", g_force[i].name); unused++; }
    if (unused && strict) die("forced takeover list is wrong");
    LOG("force: %u entries hit, %u not referenced by this game", g_nforce - unused, unused);
}

// Read the rewriter table header (segment boundaries, slot numbers); any row beyond the header means an old-format build output (code modified at load time), which is rejected
static void read_layout(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) die("open %s", path);
    static const char *kn[KEY_N] = {"K18", "KTLS", "KTLR", "KGATE"};
    char line[256], key[32]; unsigned long v; unsigned seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %31s %lx", key, &v) != 2) die("%s: the output comes from an old rewriter; rewrite the modules again with dm/tools/patch_nso.py", path);
        if (!strcmp(key, "PATCH_OFF")) m->patch_off = strtoll(line + 2 + strlen(key), NULL, 16);   // may be negative (patch area before the base)
        else if (!strcmp(key, "IMAGE_SIZE")) m->image_size = v;
        else if (!strcmp(key, "FILE_IMAGE_LEN")) m->file_image_len = v;
        else if (!strcmp(key, "KEY_PAGE_OFF")) m->key_page_off = v;
        else if (!strcmp(key, "KEY_PAGE_SIZE")) m->key_page_size = v;
        else if (!strcmp(key, "TEXT_END")) m->text_end = v;
        else if (!strcmp(key, "RO_OFF")) m->ro_off = v;
        else if (!strcmp(key, "DATA_OFF")) m->data_off = v;
        else if (!strcmp(key, "FPS_STATE_OFF")) m->fps_state_off = v;
        else if (!strcmp(key, "BUILD_ID")) sscanf(line, "# %*s %32s", m->build_id);
        else for (int k = 0; k < KEY_N; k++) {
            size_t n = strlen(kn[k]);
            if (!strncmp(key, "KEY_", 4) && !strncmp(key + 4, kn[k], n) && !strcmp(key + 4 + n, "_IDX")) {
                m->key_idx[k] = (unsigned)strtoul(strrchr(line, '\t') + 1, 0, 10); seen |= 1u << k;
            }
        }
    }
    fclose(f);
    if (!m->patch_off || m->key_page_off % 0x4000 || m->key_page_size != 0x4000 || seen != (1u << KEY_N) - 1)
        die("%s: the output comes from an old rewriter; rewrite the modules again with dm/tools/patch_nso.py", path);
    if (m->patch_off < 0 && (!m->image_size || !m->file_image_len || m->file_image_len % 0x4000 || m->patch_off % 0x4000 || m->key_page_off + m->key_page_size > m->image_size))
        die("%s: patch area before the base, but IMAGE_SIZE / FILE_IMAGE_LEN missing or wrong", path);
}

// Frame step state (the page after the key page, read-write; layout in patch_nso.py FRAME_STEP): the rewriter's stubs step the game by the host's whole vsyncs capped at this
// target, 0 = the game's own interval logic. The profile's fps_target, overridden by DM_FPS_TARGET (0, 30 or 60); the graphics layer decides the steps and paces presents
// (hle_nvn.c fstep_tick)
uint8_t *dm_fps_state;
static void install_fps_state(module_t *m) {
    if (m->fps_state_off + 0x40 > m->size || m->fps_state_off < m->key_page_off + m->key_page_size) die("%s: frame step state page out of range", m->name);
    const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET;
    if (t != 0 && t != 30 && t != 60) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 30 or 60; using the game's own frame step", e); t = 0; }
    float f = (float)t; dm_fps_state = m->base + m->fps_state_off; memcpy(dm_fps_state, &f, sizeof f);
    // Render scale of the 3D views (the stubs at Tears of the Kingdom's main+0xd5cc0, Breath of the Wild's main+0x1266880): 1.0 keeps the game's own size and dynamic resolution;
    // another factor fixes the size at that fraction (DM_RENDER_SCALE, 0.5 to 1.0). Independent of the frame step: written at target 0 too
    const char *rs = getenv("DM_RENDER_SCALE"); float sc = 1.0f;
    if (rs) { char *end; sc = strtof(rs, &end); if (end == rs || *end || !(sc >= 0.5f && sc <= 1.0f)) { LOG("[RED] DM_RENDER_SCALE=%s: must be 0.5 to 1.0; keeping the game's size", rs); sc = 1.0f; } else LOG("%s: 3D views rendered at %.2f of the game's size (DM_RENDER_SCALE)", m->name, sc); }
    memcpy(dm_fps_state + 0x10, &sc, sizeof sc);
    char cap[32]; snprintf(cap, sizeof cap, ", capped at %d fps", t);
    LOG("%s: frame step %s%s (%s)", m->name, !t ? "as the game sets it" : "in whole vsyncs decided by the host", t ? cap : "", e ? "DM_FPS_TARGET" : "game profile fps_target");
}
static void install_key_page(module_t *m) {
    if (m->key_page_off + m->key_page_size > m->size) die("%s: key page out of range", m->name);
    uint64_t *page = (uint64_t *)(m->base + m->key_page_off);
    memset(page, 0, m->key_page_size);
    #ifdef __ANDROID__
    for (unsigned i = 0; i < KEY_N; i++) page[m->key_idx[i]] = (uint64_t)(1 + i) * 8;   // offset of the slot in the thread's slot block (slot_block)
#else
    for (unsigned i = 0; i < KEY_N; i++) page[m->key_idx[i]] = (uint64_t)key_value(i) * 8;
#endif
    for (int k = 0; k < KEY_N; k++) if (!page[m->key_idx[k]]) die("%s: key page entry %d is 0 (host key not created) -- the game would read TSD slot 0", m->name, k);
    if (mprotect(m->base + m->key_page_off, m->key_page_size, PROT_READ)) die("%s key page mprotect", m->name);
    if (m->fps_state_off) install_fps_state(m);
}

static void register_gates(module_t *m, const char *dir) {
    char path[1024], line[256]; snprintf(path, sizeof path, "%s/%s.gates.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    int ng = 0;
    if (f) {
        unsigned long ret, orig; char kind[16]; int arg;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%lx %15s %d %lx", &ret, kind, &arg, &orig) == 4) { gate_register((uint64_t)m->base + ret, kind, arg, orig); ng++; }
        fclose(f);
    }
    LOG("%s: gates %d patch_off %#llx keys %lu/%lu/%lu/%lu", m->name, ng, m->patch_off, g_key18, g_keytls, g_keytlr, g_keygate);
}

// DT_RELR (from SDK 20; all three modules of The Legend of Zelda: Breath of the Wild 1.9.0 have it): a compact table of relative relocations with the addend in the slot (*slot += base).
// Each 8-byte entry: even = a new start offset (relocate it, the pointer moves to the next slot); odd = a bitmap whose bits 1-63 stand for the 63 slots after the pointer, which then moves on by 63 slots.
// Without it main's GOT and vtables hold module offsets (main's RELA keeps only symbol relocations)
static unsigned relr_apply(module_t *m, uint64_t relr, uint64_t relrsz, uint64_t delta) {
    const uint64_t *e = (const uint64_t *)(m->base + relr); uint64_t *where = NULL; unsigned n = 0;
    for (uint64_t i = 0; i < relrsz / 8; i++) {
        uint64_t v = e[i];
        if (!(v & 1)) { where = (uint64_t *)(m->base + v); *where++ += delta; n++; continue; }
        if (!where) die("%s: DT_RELR bitmap entry with no start before it", m->name);
        for (unsigned b = 0; (v >>= 1); b++) if (v & 1) { where[b] += delta; n++; }
        where += 63;
    }
    return n;
}

static void relocate(module_t *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case DT_RELR: relr = d->d_un.d_ptr; break;
        case DT_RELRSZ: relrsz = d->d_un.d_val; break;
    }
    int cnt[3] = {0}, nint = 0, nweak0 = 0;
    unsigned nrelr = relr ? relr_apply(m, relr, relrsz, (uint64_t)m->base) : 0;
    const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
    uint64_t sizes[2] = {relasz, pltsz};
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
            const Elf64_Rela *r = &tabs[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
            if (type == R_AARCH64_RELATIVE) { *where = (uint64_t)m->base + r->r_addend; cnt[0]++; continue; }
            const Elf64_Sym *s = &m->symtab[si];
            uint64_t v = (uint64_t)force_lookup(m->strtab + s->st_name);
            // Symbols the module defines itself are also looked up globally in load order (the ELF default; Horizon's rtld does the same): the definition of an earlier-loaded module overrides a later one; only local / non-default visibility binds to itself.
            // Engage's main defines its own operator new / delete (Unity's memory manager) and sdk has a copy too (forwarding to malloc / free): previously sdk bound its own delete,
            // so blocks from main's new were freed by sdk's libc++ string code into the SDK heap, and CentralHeap::Free reported NotNMallocPtr
            if (!v && s->st_shndx) {
                uint64_t self = (uint64_t)m->base + s->st_value;
                v = ELF64_ST_BIND(s->st_info) == STB_LOCAL || ELF64_ST_VISIBILITY(s->st_other) != STV_DEFAULT ? self : resolve_sym(m, m->strtab + s->st_name);
                if (!v) v = self;
                if (v != self && nint++ < 8) LOG("%s: %s bound to an earlier module's definition (%s)", m->name, m->strtab + s->st_name, module_of((void *)v) ? module_of((void *)v)->name : "?");
            }
            // An undefined weak symbol that no module defines and the host does not implement is 0, as Horizon's rtld binds it: the code tests its address before calling it.
            // Octopath Traveler 0's main imports the thread_local init function _ZTHN13LowLevelTasks5FTask10ActiveTaskE weakly; a named stub made it look defined and the game called it
            const char *nm = m->strtab + s->st_name;
            int weak0 = !v && !s->st_shndx && ELF64_ST_BIND(s->st_info) == STB_WEAK && !guest_sym(nm) && !hle_implements(nm);
            if (weak0 && nweak0++ < 8) LOG("%s: undefined weak %s bound to 0", m->name, nm);
            if (!v && !weak0) v = resolve_sym(m, nm);
            if (type == R_AARCH64_JUMP_SLOT) { *where = v; cnt[2]++; }
            else { *where = v + r->r_addend; cnt[1]++; }       // ABS64 / GLOB_DAT
        }
    LOG("%s: reloc relative=%d relr=%u abs/glob=%d jump=%d%s", m->name, cnt[0], nrelr, cnt[1], cnt[2], nint ? " (some bound to an earlier module's definition)" : "");
    if (nint > 8) LOG("%s: %d bound to an earlier module's definition in all", m->name, nint);
}

// DM_LOADHASH=1 (diagnostics): after loading, output an address-independent sha256 per module, to prove that the load results of "early start / no early start" are byte-identical.
// Module base addresses and host ASLR differ every run, and only the values of the relocation slots change with them: copy the image and, using the same relocation table, replace every slot with an address-independent token --
// RELATIVE → (0, addend); symbol target falls in some module → (1 + module number, offset within the module); otherwise (host forced take-over / host symbol) → (name FNV, addend); all other bytes as is.
#include <CommonCrypto/CommonDigest.h>
void loader_hash_log(void) {
    if (!getenv("DM_LOADHASH")) return;
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        uint8_t *cp = malloc(m->size); memcpy(cp, m->base, m->size);
        uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
        for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
            case DT_RELA: rela = d->d_un.d_ptr; break; case DT_RELASZ: relasz = d->d_un.d_val; break;
            case DT_JMPREL: jmprel = d->d_un.d_ptr; break; case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
            case DT_RELR: relr = d->d_un.d_ptr; break; case DT_RELRSZ: relrsz = d->d_un.d_val; break;
        }
        // DT_RELR slots: subtracting the base makes them address-independent (done on the copy, reusing relr_apply's walk)
        if (relr) { uint8_t *b = m->base; m->base = cp; relr_apply(m, relr, relrsz, -(uint64_t)b); m->base = b; }
        const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
        uint64_t sizes[2] = {relasz, pltsz}; unsigned nslot = 0;
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tok[1] = {0};
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset); int hit = 0;
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tok[0] = 1 + j; hit = 1; break; }
                    if (!hit) { const char *nm = m->strtab + m->symtab[ELF64_R_SYM(r->r_info)].st_name; uint64_t h = 0xcbf29ce484222325ull;
                                for (; *nm; nm++) h = (h ^ (uint8_t)*nm) * 0x100000001b3ull; tok[0] = h | 1ull << 63; }
                }
                memcpy(cp + r->r_offset, &tok[0], 8);                // The 8 slot bytes are replaced by the token head; the token tail is appended below in slot order
                nslot++;
            }
        CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, cp, (CC_LONG)m->size);
        for (int t = 0; t < 2; t++)                                // Token tails (addend / offset) are appended in slot order, and together with the slot head uniquely determine the slot value
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tail = (uint64_t)r->r_addend;
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset);
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tail = v - (uint64_t)g_mod[j].base; break; }
                }
                CC_SHA256_Update(&c, &tail, 8);
            }
        uint8_t d[32]; CC_SHA256_Final(d, &c); free(cp);
        char hx[65]; for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        LOG("DM_LOADHASH %s size %#zx relocation slots %u sha256 %s", m->name, m->size, nslot, hx);
    }
}

#if TARGET_OS_IPHONE
// iOS does not allow code loaded at run time to execute, and Apple allows an executable only one segment with execute permission (ITMS-90999): the image is compiled into this executable by dm/ios/tools/gen_embed.py --
// the two code blocks (original text, patch area) are sections in __TEXT, and W (the rest of ro + data + bss) is a separate RW segment. The three blocks are not adjacent in the file; here one contiguous address range is reserved,
// and they are vm_remap'ed back at their original relative positions: code pages with copy=FALSE (sharing the same signed pages with __TEXT, r-x), W with copy=TRUE (copy-on-write, paged only when used).
// The run-time layout is byte-identical to the original image. Remapping signed pages to a new address and then executing them on iOS is exactly what libffi's trampoline table (FFI_EXEC_TRAMPOLINE_TABLE) does
#include <mach/mach.h>
static void remap_at(module_t *m, vm_address_t to, const void *from, unsigned long size, boolean_t copy, vm_prot_t need) {
    vm_address_t a = to; vm_prot_t cur = 0, max = 0;
    kern_return_t kr = vm_remap(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)from, copy, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || a != to) die("%s vm_remap %p → %#lx (%#lx bytes) failed: %d", m->name, from, (unsigned long)to, size, kr);
    if ((cur & need) != need) die("%s permissions after vm_remap %#x, missing %#x", m->name, cur, need);
}
#define IMAGE_MH (&_mh_execute_header)
static void map_image(module_t *m, const char *dir) {
    (void)dir;
    char sx0[17], sx1[17], dm[17]; snprintf(sx0, sizeof sx0, "__dm%sx0", m->name); snprintf(sx1, sizeof sx1, "__dm%sx1", m->name); snprintf(dm, sizeof dm, "__DM%sW", m->name);
    unsigned long z0, zw, z1;
    const uint8_t *x0 = getsectiondata(IMAGE_MH, "__TEXT", sx0, &z0), *w = getsectiondata(IMAGE_MH, dm, "__img", &zw), *x1 = getsectiondata(IMAGE_MH, "__TEXT", sx1, &z1);
    if (!x0 || !w || !x1) die("embedded sections %s / %s / %s missing", sx0, dm, sx1);
    // Patch area before the base (patch_off < 0): W ends at the end of data (FILE_IMAGE_LEN); bss and the key page are zero pages from vm_allocate and are not in the executable
    int front = m->patch_off < 0;
    if (z0 + zw != (front ? m->file_image_len : (uint64_t)m->patch_off) || (z0 & 0x3FFF) || (zw & 0x3FFF) || (z1 & 0x3FFF) || ((uintptr_t)x0 & 0x3FFF) || ((uintptr_t)w & 0x3FFF) || ((uintptr_t)x1 & 0x3FFF))
        die("%s embedded section layout mismatch (x0 %#lx w %#lx x1 %#lx, PATCH_OFF %#llx)", m->name, z0, zw, z1, (unsigned long long)m->patch_off);
    m->size = front ? m->image_size : m->patch_off + z1;
    uint64_t pre = front ? (uint64_t)-m->patch_off : 0;
    if (front) m->patch_len = z1;
    vm_address_t base = 0;
    if (vm_allocate(mach_task_self(), &base, pre + m->size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) die("%s failed to reserve %#llx bytes of address space", m->name, (unsigned long long)(pre + m->size));
    base += pre;
    remap_at(m, base, x0, z0, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    remap_at(m, base + z0, w, zw, TRUE, VM_PROT_READ | VM_PROT_WRITE);
    remap_at(m, base + m->patch_off, x1, z1, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    m->base = (uint8_t *)base; install_key_page(m);
}
#else
// Code patches applied while the image is still writable, before the code area is sealed: IPSwitch .pchtxt files from DM_MODS_DIR (community mods, only the ones whose
// @nsobid matches the module), then the built-in ones. Either skips a patch that would overwrite an instruction the rewriter (patch_nso.py) turned into a branch into
// its patch area [p0, p1), since that would undo the rewrite
#include <ctype.h>
#include <dirent.h>
#include <strings.h>
static int hex_nib(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int overlaps_rewrite(const module_t *m, uint64_t off, size_t nb, int64_t p0, int64_t p1) {
    if (!nb || off >= m->text_end) return 0;
    for (uint64_t a = off & ~3ull; a <= ((off + nb - 1) & ~3ull) && a + 4 <= m->text_end; a += 4) {
        uint32_t w; memcpy(&w, m->base + a, 4);
        if ((w & 0xfc000000u) == 0x14000000u) { int64_t t = (int64_t)a + ((int64_t)(int32_t)(w << 6) >> 4); if (t >= p0 && t < p1) return 1; }
    }
    return 0;
}
// One .pchtxt file: "@nsobid-<build id>", "@flag offset_shift <n>", "@enabled" / "@disabled", "@stop", then "<hex offset> <hex bytes>" or "<hex offset> \"text\"" lines
// (offsets count the 0x100-byte NSO header, as IPSwitch's do); // and # start comments
// The 16 KB pages of the code area a .pchtxt mod wrote into, kept writable after the area is sealed: some mods keep their own variables in the code they add (Fl4sh's
//   Dynamic FPS for Legends: Arceus stores its timing there every frame; the Switch emulators allow it, a sealed page closed the game at its first write).
//   DM_MODS_RWX=0 seals them like the rest
static struct { const module_t *m; uint64_t pg; } g_mod_pg[64]; static int g_nmod_pg;
static void mod_page_note(const module_t *m, uint64_t off, size_t nb) {
    for (uint64_t pg = off & ~0x3FFFull; pg < off + nb; pg += 0x4000) {
        int k = 0; while (k < g_nmod_pg && !(g_mod_pg[k].m == m && g_mod_pg[k].pg == pg)) k++;
        if (k == g_nmod_pg && g_nmod_pg < 64) { g_mod_pg[g_nmod_pg].m = m; g_mod_pg[g_nmod_pg].pg = pg; g_nmod_pg++; } }
}
static void mod_pages_open(const module_t *m, uint64_t rx) {
    const char *e = getenv("DM_MODS_RWX"); if (e && !atoi(e)) return;
    int n = 0;
    // (pages outside the code area too: Fl4sh's Ultrawide mods for Legends: Arceus put their added code in a gap of the data area, +0x38c851c, which the
    //   emulators run as code; here it was data, not executable, and the game closed at its first call)
    for (int k = 0; k < g_nmod_pg; k++) if (g_mod_pg[k].m == m && g_mod_pg[k].pg + 0x4000 <= m->size) {
        if (mprotect(m->base + g_mod_pg[k].pg, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC)) LOG("[RED] %s: +%#llx: a modded code page could not be made writable (%s)", m->name, (unsigned long long)g_mod_pg[k].pg, strerror(errno));
        else n++; }
    if (n) LOG("%s: %d page(s) a mod patched stay writable and executable, for mods that keep their variables or code there (DM_MODS_RWX=0 seals them)", m->name, n);
}
static int pchtxt_apply(module_t *m, uint64_t lim, int64_t p0, int64_t p1, const char *path) {
    if (!m->build_id[0]) return 0;
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char ln[1024]; int bid_ok = 0, en = 0, applied = 0, line_no = 0; int64_t shift = 0;
    while (fgets(ln, sizeof ln, f)) {
        char *t = ln; if (!line_no++ && (uint8_t)t[0] == 0xef && (uint8_t)t[1] == 0xbb && (uint8_t)t[2] == 0xbf) t += 3;
        char *c1 = strstr(t, "//"), *c2 = strchr(t, '#'); if (c1) *c1 = 0; if (c2) *c2 = 0;
        while (isspace((unsigned char)*t)) t++;
        for (size_t e = strlen(t); e && isspace((unsigned char)t[e - 1]); ) t[--e] = 0;
        if (!*t) continue;
        if (*t == '@') {
            if (!strncasecmp(t, "@nsobid-", 8)) { const char *b = t + 8; size_t bl = 0; while (hex_nib(b[bl]) >= 0) bl++;
                if (bl < 16 || strncasecmp(b, m->build_id, bl < 32 ? bl : 32)) { fclose(f); return 0; }   // another module or version
                bid_ok = 1; }
            else if (!strncasecmp(t, "@flag", 5) && isspace((unsigned char)t[5])) { char *q = t + 5; while (isspace((unsigned char)*q)) q++;
                if (!strncasecmp(q, "offset_shift", 12) && isspace((unsigned char)q[12])) shift = strtoll(q + 12, NULL, 0); }
            else if (!strcasecmp(t, "@enabled")) en = 1;
            else if (!strcasecmp(t, "@disabled")) en = 0;
            else if (!strcasecmp(t, "@stop")) break;
            continue;
        }
        if (!bid_ok || !en) continue;
        char *end = NULL; uint64_t addr = strtoull(t, &end, 16);
        if (end == t || !isspace((unsigned char)*end)) continue;
        char *v = end; while (isspace((unsigned char)*v)) v++;
        uint8_t buf[512]; size_t nb = 0; int bad = 0;
        if (*v == '"') { for (v++; *v && *v != '"'; v++) { if (*v == '\\' && v[1]) v++; if (nb >= sizeof buf) { bad = 1; break; } buf[nb++] = (uint8_t)*v; } if (*v != '"') bad = 1; }
        else while (*v) { if (isspace((unsigned char)*v)) { v++; continue; } int hi = hex_nib(v[0]), lo = v[1] ? hex_nib(v[1]) : -1;
            if (hi < 0 || lo < 0 || nb >= sizeof buf) { bad = 1; break; } buf[nb++] = (uint8_t)(hi << 4 | lo); v += 2; }
        if (bad || !nb) { LOG("[RED] %s:%d: not a valid value, skipped", path, line_no); continue; }
        int64_t off = (int64_t)addr + shift - 0x100;
        if (off < 0 || (uint64_t)off + nb > lim) { LOG("[RED] %s:%d: +%#llx (%zu bytes) is outside %s, skipped", path, line_no, (unsigned long long)off, nb, m->name); continue; }
        if (overlaps_rewrite(m, (uint64_t)off, nb, p0, p1)) { LOG("[RED] %s:%d: +%#llx is code the host rewrote, skipped", path, line_no, (unsigned long long)off); continue; }
        memcpy(m->base + off, buf, nb); applied++; mod_page_note(m, (uint64_t)off, nb);
    }
    fclose(f);
    if (applied) LOG("%s: %d patch(es) from %s", m->name, applied, path);
    return applied;
}
// the .pchtxt files of one folder, in name order; depth 1 also looks into each mod folder as the Switch emulators lay them out (<mod>/exefs/*.pchtxt, or <mod>/*.pchtxt);
// a mod folder named with a leading "-" or "." is switched off
static int pchtxt_scan(module_t *m, uint64_t lim, int64_t p0, int64_t p1, const char *d, int depth, int *files) {
    struct dirent **ents = NULL; int n = scandir(d, &ents, NULL, alphasort), total = 0;
    if (n < 0) { if (errno != ENOENT && depth) LOG("[RED] %s: mods folder %s unreadable (errno %d)", m->name, d, errno); return 0; }
    for (int i = 0; i < n; i++) { const char *nm = ents[i]->d_name; size_t l = strlen(nm); char path[1200]; snprintf(path, sizeof path, "%s/%s", d, nm);
        struct stat st;
        if (l > 7 && !strcasecmp(nm + l - 7, ".pchtxt")) { (*files)++; total += pchtxt_apply(m, lim, p0, p1, path); }
        else if (depth > 0 && nm[0] != '.' && nm[0] != '-' && strcasecmp(nm, "romfs") && !stat(path, &st) && S_ISDIR(st.st_mode)) {
            char ex[1300]; snprintf(ex, sizeof ex, "%s/exefs", path);
            total += pchtxt_scan(m, lim, p0, p1, ex, 0, files) + pchtxt_scan(m, lim, p0, p1, path, 0, files);
        }
        free(ents[i]); }
    free(ents);
    return total;
}
static int pchtxt_dir(module_t *m, uint64_t lim, int64_t p0, int64_t p1) {
    const char *d = getenv("DM_MODS_DIR"); if (!d || !*d || !m->build_id[0]) return 0;
    int files = 0, total = pchtxt_scan(m, lim, p0, p1, d, 1, &files);
    if (files) LOG("%s: mods folder %s: %d .pchtxt file(s), %d patch(es) applied to this module", m->name, d, files, total);
    return total;
}
// Built-in patches. (The 16:9 that PR #48 patched alone, MOVK X9, #0x8e39 / #0x3fe3 at +0x607de8, is a camera object's default aspect, which the game replaces
// for the gameplay camera: patched alone, the picture was unchanged. wide_patch below also patches where the gameplay camera takes it, and the culling.)
int dm_step_hz; uint8_t *dm_step_word;   // the high frame rate below: its rate and the game's step word, which hle_nvn.c vstep_tick rewrites every frame
static int fps_patch(module_t *m, uint64_t lim, int64_t p0, int64_t p1);
// Built-in code patches as .pchtxt files (builtin_mods.h, from dm/games/*/builtin_mods/*.pchtxt), each behind a setting: Scarlet / Violet 4.0.0's shadow map size,
// 2048×2048 with DM_SHADOWS=2k (test only), 512×512 with DM_SHADOWS=low. A patch goes in whole or not at all: every site must still hold the game's own bytes and none
// may be code the rewriter turned into a branch
#include "builtin_mods.h"
static int bmod_wanted(const char *name) {
    // the shadow map's size where the game makes it (main+0x146e80c MOVZ W8, #0x400: 1024): 2048 (a bigger map takes the game's GPU pool from streaming: test only,
    //   not offered in the menu)
    if (!strcmp(name, "shadows_high")) { const char *e = getenv("DM_SHADOWS"); return e && !strcasecmp(e, "2k"); }
    // DM_SHADOWS=low: the same site, 512×512 instead of the game's 1024 (a quarter of the shadow map's pixels; a smaller map also leaves the game's GPU
    //   pool more room, where a bigger one takes it from streaming). Not offered in the menu until seen on a phone
    if (!strcmp(name, "shadows_low")) { const char *e = getenv("DM_SHADOWS"); return e && !strcasecmp(e, "low"); }
    return 0;
}
int dm_shadow_off;   // nvn_vk.c draw: DM_SHADOWS=off on Scarlet / Violet 4.0.0's main, the sun shadow pass's draws dropped
static int bmod_apply(module_t *m, uint64_t lim, int64_t p0, int64_t p1) {
    int total = 0;
    for (unsigned i = 0; i < sizeof BUILTIN_MODS / sizeof *BUILTIN_MODS; i++) {
        const bmod *b = &BUILTIN_MODS[i]; int hit = 0;
        for (int k = 0; b->builds[k]; k++) if (!strncasecmp(m->build_id, b->builds[k], 32)) hit = 1;
        if (hit && !strcmp(b->name, "shadows_low") && !dm_shadow_off) { const char *e = getenv("DM_SHADOWS");   // (the shadow patches' builds are Scarlet / Violet 4.0.0)
            if (e && !strcasecmp(e, "off")) { dm_shadow_off = 1; LOG("%s: shadows off (DM_SHADOWS=off): the sun shadow pass's draws are dropped", m->name); } }
        if (!hit || !bmod_wanted(b->name)) continue;
        int bad = -1;
        for (int s = 0; s < b->n && bad < 0; s++) { const bmod_site *t = &b->sites[s];
            if ((uint64_t)t->off + t->len > lim || memcmp(m->base + t->off, t->orig, t->len) || overlaps_rewrite(m, t->off, t->len, p0, p1)) bad = s; }
        if (bad >= 0) { LOG("[RED] %s: built-in patch %s not applied: +%#x differs from the game's code or was rewritten", m->name, b->name, b->sites[bad].off); continue; }
        for (int s = 0; s < b->n; s++) memcpy(m->base + b->sites[s].off, b->sites[s].mod, b->sites[s].len);
        LOG("%s: built-in patch %s applied (%d sites)", m->name, b->name, b->n); total += b->n;
    }
    return total;
}
// Scarlet / Violet 4.0.0 up to 60 fps (DM_FPS_TARGET=60, the menu's 60 fps and Dynamic): our patch, paldea_fps.h (what each site is in the game's code). The host
//   presents in whole vsyncs and writes the step the frame really took into the game's frame-time table (hle_nvn.c dyn_tick, dm_dyn_step)
#include "paldea_fps.h"
int dm_dynfps;   // hle_nvn.c dyn_tick: Scarlet / Violet at up to 60, presentation paced in steps of 30 / 60
uint8_t *dm_dyn_step;   // hle_nvn.c dyn_tick: the game's step word (u64 ns) it writes every frame
static int paldea_fps_patch(module_t *m, uint64_t lim, int64_t p0, int64_t p1) {
    if (!pfps_build(m->build_id) || !pfps_wanted(getenv("DM_FPS_TARGET"))) return 0;   // (at 30 nothing is written: the game's own code)
    int bad = pfps_check(m->base, lim, m->text_end);
    for (int i = 0; i < PFPS_NCODE && bad < 0; i++) if (overlaps_rewrite(m, PFPS_CODE[i].off, 4, p0, p1)) bad = i;
    if (bad >= 0) { LOG("[RED] %s: 60 fps not applied: +%#llx differs from Scarlet / Violet 4.0.0's code or was rewritten; the game stays at its own 30", m->name,
                        (unsigned long long)(bad < PFPS_NCODE ? PFPS_CODE[bad].off : PFPS_STEP)); return 0; }
    pfps_write_code(m->base);
    dm_dyn_step = m->base + PFPS_STEP; dm_dynfps = 1;
    LOG("%s: up to 60 fps for Scarlet / Violet 4.0.0: present interval 1 (+%#x), the step the frame took in the game's frame-time table (+%#llx, 16.67 or 33.33 ms), "
        "frame-time divisor kept at 30 fps (+%#x)", m->name, PFPS_CODE[0].off, (unsigned long long)PFPS_STEP, PFPS_CODE[1].off);
    return PFPS_NCODE;
}
// Widescreen (DM_ASPECT=wide, the menu's Picture → Widescreen): the game's camera made as wide as the screen, for Sword and Shield 1.3.2, after Fl4sh's Ultrawide
//   mods (16:10 / 21:9 / 32:9 .pchtxt), with this screen's own aspect (DM_WIDE_RATIO, its long side over its short, written by the app) instead of a fixed one.
//   The frame stays 1280x720 (the picture fills the screen: nvn_ovl.h); what the camera sees is widened, so nothing is stretched. Three places:
//   - the gameplay camera copies its parameters from the scene's (Shield / Sword +0x607670): the aspect, LDR W9, [X1, #0xd4] at +0x6076c4, becomes a branch into
//     the three free words after the function before (+0x607664: MOV W9, #lo; MOVK W9, #hi, LSL #16; B back);
//   - a camera object's default aspect, MOVK X9 #0x8e39 / #0x3fe3 (16:9) at +0x607de8 / +0x607dec;
//   - culling (Shield +0xd93444, Sword +0xd93414): LDP X12, X13, [X0, #0xd0] takes the frustum's aspect into the copy on the stack, of which only the low word (the
//     aspect) is ever read. Fl4sh's mod sets the top half of whatever X12 held (MOVK): here MOVZ W12, the aspect rounded up to its top 16 bits (culls a hair
//     wider than it sees, never narrower), the same every frame.
//   Every word is checked first (the three free words zero, the rest the game's own); a build or code that differs is left alone. The HUD is drawn in the same
//   16:9 frame and is stretched with it: a widescreen mod's UI fix would be a separate patch
static const struct { const char *build, *game; uint64_t cull; } WIDE_SITES[] = {
    { "a16802625e7826bf83b6f9708e475b91", "Shield 1.3.2", 0xd93444 },
    { "a3b75bcd3311385aeed67fbeeb79cbb7", "Sword 1.3.2", 0xd93414 },
};
// The HUD kept 16:9 in the middle with Widescreen (DM_WIDE_HUD=0: stretched as before). Every ui2d layout's 2D camera takes its projection from a param block
//   {u32 type, l, r, b, t, near, far} (SetProjection main+0x6851c0 → matrix main+0x684ef0, type 2 = orthographic, one path for every 2D camera): nine HUD blocks
//   {2, -960, 960, -540, 540, 0.1, 800} and the default {2, -640, 640, -360, 360, ...} at main+0x203fac8, in read-only data at the same addresses in Sword and Shield
//   1.3.2. Their left / right times k = the screen's ratio / (16/9): the layout lands 16:9 in the middle of the wider picture, whether the game renders 16:9 (stretched
//   to the screen) or the screen's own shape (DM_WIDE_RENDER). Each block's 28 bytes are checked first. Not yet seen on a phone: full-screen fades and menu
//   backgrounds now cover the middle 16:9 only
static int wide_hud_patch(module_t *m, double ratio, const char *game) {
    const char *e = getenv("DM_WIDE_HUD"); if (e && !strcmp(e, "0")) return 0;
    static const uint64_t B[10] = { 0x205f484, 0x2060404, 0x2060420, 0x2060458, 0x20604d4, 0x20620b0, 0x2062724, 0x2068a88, 0x2074650, 0x203fac8 };
    static const float P1080[7] = { 0, -960.0f, 960.0f, -540.0f, 540.0f, 0.1f, 800.0f }, P720[7] = { 0, -640.0f, 640.0f, -360.0f, 360.0f, 0.1f, 800.0f };
    float k = (float)(ratio / (16.0 / 9.0)); int n = 0;
    for (unsigned i = 0; i < 10; i++) {
        const float *want = i == 9 ? P720 : P1080; uint8_t *p = m->base + B[i]; uint32_t type;
        if (B[i] + 28 > m->size) continue;
        memcpy(&type, p, 4); if (type != 2 || memcmp(p + 4, want + 1, 24)) continue;
        float lr[2] = { want[1] * k, want[2] * k }; memcpy(p + 4, lr, 8); n++;
    }
    if (n == 10) LOG("%s: widescreen HUD for %s: kept 16:9 in the middle (the 2D layouts' projection x %.4f)", m->name, game, (double)k);
    else LOG("[RED] %s: widescreen HUD for %s: %d of 10 layout projections matched (the rest stay stretched)", m->name, game, n);
    return n;
}
static int wide_patch(module_t *m, uint64_t lim, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_ASPECT"); if (!e || strcmp(e, "wide")) return 0;
    const char *r = getenv("DM_WIDE_RATIO"); double ratio = r ? atof(r) : 0;
    int si = -1; for (unsigned i = 0; i < sizeof WIDE_SITES / sizeof *WIDE_SITES; i++) if (!strncasecmp(m->build_id, WIDE_SITES[i].build, 32)) si = (int)i;
    if (si < 0) { LOG("[RED] %s: widescreen not applied (not Pokémon Sword / Shield 1.3.2's main)", m->name); return 0; }
    if (ratio < 1.0 || ratio > 4.0) { LOG("[RED] %s: widescreen not applied (no screen aspect: DM_WIDE_RATIO %s)", m->name, r ? r : "not set"); return 0; }
    if (ratio > 16.0 / 9.0 - 0.01 && ratio < 16.0 / 9.0 + 0.01) { LOG("%s: widescreen: this screen is 16:9 already", m->name); return 0; }
    const uint64_t cave = 0x607664, ld = 0x6076c4, def = 0x607de8, cull = WIDE_SITES[si].cull;
    static const uint32_t orig_ld = 0xb940d429u, orig_def[2] = { 0xf2d1c729u, 0xf2e7fc69u }, orig_cull = 0xa94d340cu, zero[3] = { 0, 0, 0 };
    if (cull + 4 > m->text_end || memcmp(m->base + cave, zero, 12) || memcmp(m->base + ld, &orig_ld, 4) || memcmp(m->base + def, orig_def, 8) || memcmp(m->base + cull, &orig_cull, 4) ||
        overlaps_rewrite(m, cave, 12, p0, p1) || overlaps_rewrite(m, ld, 4, p0, p1) || overlaps_rewrite(m, def, 8, p0, p1) || overlaps_rewrite(m, cull, 4, p0, p1)) {
        LOG("[RED] %s: widescreen not applied (%s: the camera or culling code differs, or was rewritten)", m->name, WIDE_SITES[si].game); return 0; }
    float f = (float)ratio; uint32_t b; memcpy(&b, &f, 4); uint32_t lo = b & 0xffff, hi = b >> 16, hc = (b + 0xffffu) >> 16;
    uint32_t w[3] = { 0x52800000u | lo << 5 | 9, 0x72a00000u | hi << 5 | 9, 0x14000000u | (uint32_t)(((int64_t)(ld + 4) - (int64_t)(cave + 8)) >> 2 & 0x3ffffff) };   // MOV W9 / MOVK W9 / B back
    uint32_t br = 0x14000000u | (uint32_t)(((int64_t)cave - (int64_t)ld) >> 2 & 0x3ffffff);
    uint32_t dw[2] = { 0xf2c00000u | lo << 5 | 9, 0xf2e00000u | hi << 5 | 9 };   // MOVK X9, #lo, LSL #32 / MOVK X9, #hi, LSL #48
    uint32_t cw = 0x52a00000u | hc << 5 | 12;   // MOVZ W12, #hc, LSL #16
    memcpy(m->base + cave, w, 12); memcpy(m->base + ld, &br, 4); memcpy(m->base + def, dw, 8); memcpy(m->base + cull, &cw, 4);
    LOG("%s: widescreen camera for %s: aspect %.4f (%08x), culling %.4f, at +%#llx / +%#llx / +%#llx", m->name, WIDE_SITES[si].game, ratio, b,
        (double)({ uint32_t c = hc << 16; float g; memcpy(&g, &c, 4); g; }), (unsigned long long)ld, (unsigned long long)def, (unsigned long long)cull);
    return 4 + wide_hud_patch(m, ratio, WIDE_SITES[si].game);
}
// True aspect ratio (experimental, DM_WIDE_RENDER=1 with Widescreen): the game renders at the screen's own shape instead of 16:9 stretched to it, so pixels stay
//   square. The graphics config's constructor (main+0x5e85e0, and an inlined copy at +0x5e86e0; the same in both games) packs each window size into one register:
//   MOVZ X9, #1920 then MOVK X9, #1080, LSL #32 (TV mode) and MOVK X9, #1280, LSL #32 after the format word (handheld). The two widths become 1080 x the screen's aspect
//   and 720 x it (even, at most 21.6:9); the heights stay, so the camera (wide_patch) sees as much more as the picture is wider. Render targets grow by the same factor
//   (about 25 % at 20:9): the game's GPU memory areas are not resized, so this stays off by default until a phone has played with it (Legends: Arceus's 1440p could not start)
static int wide_render_patch(module_t *m, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_WIDE_RENDER"), *a = getenv("DM_ASPECT"), *r = getenv("DM_WIDE_RATIO");
    if (!e || atoi(e) <= 0 || !a || strcmp(a, "wide")) return 0;
    int si = -1; for (unsigned i = 0; i < sizeof WIDE_SITES / sizeof *WIDE_SITES; i++) if (!strncasecmp(m->build_id, WIDE_SITES[i].build, 32)) si = (int)i;
    double ratio = r ? atof(r) : 0; if (si < 0 || ratio < 16.0 / 9.0 + 0.01) return 0;
    if (ratio > 2.4) ratio = 2.4;
    uint32_t w1080 = (uint32_t)(1080 * ratio / 2 + 0.5) * 2, w720 = (uint32_t)(720 * ratio / 2 + 0.5) * 2;
    static const struct { uint64_t off; uint32_t orig; int tv; } S[] = { { 0x5e860c, 0xd280f009u, 1 }, { 0x5e861c, 0xf2c0a009u, 0 }, { 0x5e871c, 0xd280f009u, 1 }, { 0x5e8734, 0xf2c0a009u, 0 } };
    for (unsigned i = 0; i < 4; i++) { uint32_t w; memcpy(&w, m->base + S[i].off, 4);
        if (S[i].off + 4 > m->text_end || w != S[i].orig || overlaps_rewrite(m, S[i].off, 4, p0, p1)) { LOG("[RED] %s: true aspect ratio not applied (%s: the graphics config code differs at +%#llx)", m->name, WIDE_SITES[si].game, (unsigned long long)S[i].off); return 0; } }
    for (unsigned i = 0; i < 4; i++) { uint32_t w = S[i].tv ? 0xd2800000u | w1080 << 5 | 9 : 0xf2c00000u | w720 << 5 | 9; memcpy(m->base + S[i].off, &w, 4); }
    LOG("%s: true aspect ratio for %s: the game renders %ux1080 in TV mode, %ux720 handheld (screen %.4f), instead of 16:9 stretched", m->name, WIDE_SITES[si].game, w1080, w720, ratio);
    return 4;
}
// Resolution (DM_GALAR_RES, Graphics → Resolution: 720 / 1080 the game's own / 1440) for Pokémon Sword and Shield 1.3.2: the TV-mode window size the graphics
//   config's constructor packs into one register, MOVZ X9, #1920 then MOVK X9, #1080, LSL #32 (main+0x5e860c / +0x5e8610, and the inlined copy at +0x5e871c / +0x5e8728;
//   the same words in both games). Handheld mode keeps its own 1280x720. Checked word by word; with it the true aspect ratio (wide_render_patch) is left out (same words).
//   1440p's render targets are 78 % larger: not yet run on a phone
static int galar_res_patch(module_t *m, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_GALAR_RES"); int res = e ? atoi(e) : 0; if (res != 720 && res != 1440) return 0;
    uint32_t W = res == 1440 ? 2560 : 1280, H = res == 1440 ? 1440 : 720;
    static const struct { uint64_t off; uint32_t orig; int h; } S[] = { { 0x5e860c, 0xd280f009u, 0 }, { 0x5e8610, 0xf2c08709u, 1 }, { 0x5e871c, 0xd280f009u, 0 }, { 0x5e8728, 0xf2c08709u, 1 } };
    for (unsigned i = 0; i < 4; i++) { uint32_t w; memcpy(&w, m->base + S[i].off, 4);
        if (S[i].off + 4 > m->text_end || w != S[i].orig || overlaps_rewrite(m, S[i].off, 4, p0, p1)) { LOG("[RED] %s: resolution %dp not applied (the graphics config code differs at +%#llx)", m->name, res, (unsigned long long)S[i].off); return 0; } }
    for (unsigned i = 0; i < 4; i++) { uint32_t w = (S[i].orig & ~(0xffffu << 5)) | (S[i].h ? H : W) << 5; memcpy(m->base + S[i].off, &w, 4); }
    LOG("%s: resolution: the TV-mode window is %ux%u (DM_GALAR_RES; handheld keeps 1280x720)", m->name, W, H);
    return 4;
}
// Widescreen for Legends: Arceus 1.1.1 (DM_ASPECT=wide, the menu's Picture → Widescreen; off by default): after Fl4sh's Ultrawide mods for 1.1.1, with this screen's own
//   aspect (DM_WIDE_RATIO, its long side over its short, written by the app) instead of a fixed 21:9. The picture fills the screen (nvn_ovl.h) and the camera sees
//   as much wider, so nothing is stretched:
//   - six cameras' 16:9 (MOV Wd, #0x8e39; MOVK Wd, #0x3fe3, LSL #16 = 1.7778f) become the screen's aspect;
//   - the HUD: the function at +0x2abe864.. hands back a layer's box (x, y, w, h, read by LDP S2, S3 at +0x2abe898); a branch there goes to code added in a gap of
//     zeros in the data area (+0x38c851c, where Fl4sh's mod puts its own), which narrows the box to 16:9 of its height, centred (w' = w x (16/9) / aspect,
//     x' = x + (w - w') / 2), so the HUD keeps its shape in the middle of the wider picture. Fl4sh's writes a fixed box for 1080p; this follows the box the game
//     gives, at any resolution. That page is kept writable and executable (mod_pages_open), as the emulators run it.
//   Every word is checked first (the cameras' 16:9, the LDP, the gap's zeros); a build or code that differs is left alone
static int arc_wide_cull(module_t *m, uint64_t lim, int64_t p0, int64_t p1, double ratio);
static int arc_wide(module_t *m, uint64_t lim, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_ASPECT"); if (!e || strcmp(e, "wide")) return 0;
    const char *r = getenv("DM_WIDE_RATIO"); double ratio = r ? atof(r) : 0;
    if (ratio < 1.0 || ratio > 4.0) { LOG("[RED] %s: widescreen not applied (no screen aspect: DM_WIDE_RATIO %s)", m->name, r ? r : "not set"); return 0; }
    if (ratio < 16.0 / 9.0 + 0.01) { LOG("%s: widescreen: this screen is not wider than 16:9 (%.4f)", m->name, ratio); return 0; }
    static const uint64_t cam[6] = { 0x1261b98, 0x191ddd0, 0x19336bc, 0x2750654, 0x124edfc, 0x194a65c };
    const uint64_t hook = 0x2abe898, ret = 0x2abe8d4, cave = 0x38c851c;
    int ok = cave + 0x24 <= lim && hook < m->text_end;
    for (int i = 0; ok && i < 6; i++) { uint32_t a, b; memcpy(&a, m->base + cam[i], 4); memcpy(&b, m->base + cam[i] + 4, 4);
        ok = (a & ~31u) == 0x5291c720u && (b & ~31u) == 0x72a7fc60u && (a & 31) == (b & 31) && !overlaps_rewrite(m, cam[i], 8, p0, p1); }
    uint32_t w; if (ok) { memcpy(&w, m->base + hook, 4); ok = w == 0x2d418d02u && !overlaps_rewrite(m, hook, 4, p0, p1); }   // (LDP S2, S3, [X8, #0xc])
    for (int i = 0; ok && i < 9; i++) { memcpy(&w, m->base + cave + 4 * i, 4); ok = !w; }
    if (!ok) { LOG("[RED] %s: widescreen not applied (Legends: Arceus 1.1.1's camera or HUD code differs, or was rewritten)", m->name); return 0; }
    float f = (float)ratio; uint32_t bits; memcpy(&bits, &f, 4); uint32_t lo = bits & 0xffff, hi = bits >> 16;
    for (int i = 0; i < 6; i++) { uint32_t rd; memcpy(&rd, m->base + cam[i], 4); rd &= 31;
        uint32_t ww[2] = { 0x52800000u | lo << 5 | rd, 0x72a00000u | hi << 5 | rd }; memcpy(m->base + cam[i], ww, 8); mod_page_note(m, cam[i], 8); }
    float k = (float)(16.0 / 9.0 / ratio);
    uint32_t c[9] = { 0x2d418d02u,   // LDP S2, S3, [X8, #0xc]   (the game's own)
                      0x1c0000e4u,   // LDR S4, k                 (the word after the B)
                      0x1e240845u,   // FMUL S5, S2, S4           w'
                      0x1e253846u,   // FSUB S6, S2, S5           w - w'
                      0x1e2c1007u,   // FMOV S7, #0.5
                      0x1f0700c0u,   // FMADD S0, S6, S7, S0      x' = x + (w - w') / 2
                      0x1e2040a2u,   // FMOV S2, S5
                      0x14000000u | (uint32_t)(((int64_t)ret - (int64_t)(cave + 28)) >> 2 & 0x3ffffff),   // B the function's return, as the game's next instruction
                      0 };
    memcpy(&c[8], &k, 4);
    memcpy(m->base + cave, c, sizeof c); mod_page_note(m, cave, sizeof c);
    uint32_t br = 0x14000000u | (uint32_t)(((int64_t)cave - (int64_t)hook) >> 2 & 0x3ffffff); memcpy(m->base + hook, &br, 4); mod_page_note(m, hook, 4);
    LOG("%s: widescreen for Legends: Arceus 1.1.1: camera aspect %.4f (%08x), HUD kept to 16:9 in the middle (x %.3f of the width)", m->name, ratio, bits, (double)k);
    { const char *wc = getenv("DM_WIDE_CULL"); if (!(wc && !strcmp(wc, "0"))) return 8 + arc_wide_cull(m, lim, p0, p1, ratio); }   // (DM_WIDE_CULL=0: the game's culling as it is)
    return 8;
}
// Widescreen culling for Legends: Arceus 1.1.1 (with arc_wide; characters and objects popped in and out at the sides of the wider picture). Two parts, every word checked:
//   - the two 16:9 defaults arc_wide's six cameras miss (the Main camera's ctor, main+0x2992488 / +0x299248c, and the camera registry entry's, +0x2a10de0 / +0x2a10de4:
//     MOVK Xd, #0x8e39, LSL #32; MOVK Xd, #0x3fe3, LSL #48 = {fov, 1.7778f}) take the screen's aspect: a Main camera made by the fallback path (0x19b1b6c) is no longer 16:9;
//   - the horizontal planes of both CPU culls (the render views' frustum, main+0x2abce48: FDIV S2, S0, S2 at +0x2abceb4; the terrain's, +0x246dc04: FDIV S2, S7, S1 at
//     +0x246dc9c; perspective only, their orthographic branches come first) are widened by 1/k, k = (16/9) / aspect, in code added in the same gap of zeros as the
//     HUD's (+0x38c8540..+0x38c8563): a little more is drawn off the sides, nothing visible pops. V16 is scratch in both functions
static int arc_wide_cull(module_t *m, uint64_t lim, int64_t p0, int64_t p1, double ratio) {
    static const uint64_t def[4] = { 0x2992488, 0x299248c, 0x2a10de0, 0x2a10de4 };
    static const uint32_t defw[4] = { 0xf2d1c729u, 0xf2e7fc69u, 0xf2d1c728u, 0xf2e7fc68u };
    const uint64_t s1 = 0x2abceb4, s2 = 0x246dc9c, cave = 0x38c8540; const uint32_t o1 = 0x1e221802u, o2 = 0x1e2118e2u;
    int ok = cave + 0x24 <= lim;
    for (int i = 0; ok && i < 4; i++) { uint32_t w; memcpy(&w, m->base + def[i], 4); ok = def[i] + 4 <= m->text_end && w == defw[i] && !overlaps_rewrite(m, def[i], 4, p0, p1); }
    uint32_t w; if (ok) { memcpy(&w, m->base + s1, 4); ok = w == o1 && !overlaps_rewrite(m, s1, 4, p0, p1); }
    if (ok) { memcpy(&w, m->base + s2, 4); ok = w == o2 && !overlaps_rewrite(m, s2, 4, p0, p1); }
    for (int i = 0; ok && i < 9; i++) { memcpy(&w, m->base + cave + 4 * i, 4); ok = !w; }
    if (!ok) { LOG("[RED] %s: widescreen culling not applied (Legends: Arceus 1.1.1's camera or culling code differs, or was rewritten)", m->name); return 0; }
    float f = (float)ratio; uint32_t bits; memcpy(&bits, &f, 4); uint32_t lo = bits & 0xffff, hi = bits >> 16;
    for (int i = 0; i < 4; i++) { uint32_t nw = (i & 1 ? 0xf2e00000u | hi << 5 : 0xf2c00000u | lo << 5) | (defw[i] & 31); memcpy(m->base + def[i], &nw, 4); mod_page_note(m, def[i], 4); }
    float k = (float)(16.0 / 9.0 / ratio);
    uint32_t c[9] = { 0x1c000110u,   // LDR S16, k             (+0x20)
                      0x1e300842u,   // FMUL S2, S2, S16
                      o1,            // FDIV S2, S0, S2        (the game's own)
                      0x14000000u | (uint32_t)(((int64_t)s1 + 4 - (int64_t)(cave + 12)) >> 2 & 0x3ffffff),
                      0x1c000090u,   // LDR S16, k             (+0x20 from cave, +0x10 from here)
                      0x1e300821u,   // FMUL S1, S1, S16
                      o2,            // FDIV S2, S7, S1        (the game's own)
                      0x14000000u | (uint32_t)(((int64_t)s2 + 4 - (int64_t)(cave + 28)) >> 2 & 0x3ffffff),
                      0 };
    memcpy(&c[8], &k, 4);
    memcpy(m->base + cave, c, sizeof c); mod_page_note(m, cave, sizeof c);
    uint32_t b1 = 0x14000000u | (uint32_t)(((int64_t)cave - (int64_t)s1) >> 2 & 0x3ffffff), b2 = 0x14000000u | (uint32_t)(((int64_t)(cave + 16) - (int64_t)s2) >> 2 & 0x3ffffff);
    memcpy(m->base + s1, &b1, 4); mod_page_note(m, s1, 4); memcpy(m->base + s2, &b2, 4); mod_page_note(m, s2, 4);
    LOG("%s: widescreen culling: default cameras at the screen's aspect, both culls %.0f %% wider", m->name, 100.0 / k);
    return 6;
}
// The render-target pool of Legends: Arceus 1.1.1, grown for a bigger render size (1440p, the screen's shape). The config builder (main+0x277d798) sets three NVN pools that
//   graphics setup (main+0x29d6a64 → 0x2aa24c4 → the pool ctor main+0x2a8cf9c) creates: config+0x28 300 MB general, config+0x30 250 MB render targets (CPU no access,
//   GPU cached, compressible), config+0x38 40 MB commands. Their storage comes from the GFX2 heap (main+0x2626d50, 800 MB), which holds them plus the NVN driver's
//   63 MB (config+0x08 / +0x10: driver memory, NOT a render-target block: the old 15 → 32 MB change there is gone) and ~147 MB of other users, with no NULL check on
//   the storage: so GFX2 grows by exactly what the pool grows. Both MOVZ Wn, #imm, LSL #16, each word checked first. 1 applied
static int arc_gpu_pool(module_t *m, unsigned pool_mb, int64_t p0, int64_t p1) {
    if (pool_mb <= 250) return 1;
    if (pool_mb > 600) pool_mb = 600;
    static const struct { uint64_t off; uint32_t orig; unsigned mb; } G[] = { { 0x277d7f0, 0x52a1f409u, 250 }, { 0x2626d50, 0x52a64001u, 800 } };
    for (unsigned i = 0; i < 2; i++) { uint32_t w; memcpy(&w, m->base + G[i].off, 4);
        if (G[i].off + 4 > m->text_end || w != G[i].orig || overlaps_rewrite(m, G[i].off, 4, p0, p1)) return 0; }
    for (unsigned i = 0; i < 2; i++) { unsigned mb = G[i].mb + (pool_mb - 250); uint32_t w = (G[i].orig & ~(0xffffu << 5)) | (mb * 16u) << 5;
        memcpy(m->base + G[i].off, &w, 4); mod_page_note(m, G[i].off, 4); }
    LOG("%s: GPU memory for the bigger render size: render-target pool %u MB (was 250), GFX2 heap %u MB (was 800)", m->name, pool_mb, 800 + pool_mb - 250);
    return 1;
}
// 1440p for Legends: Arceus 1.1.1 (DM_ARC_RES=1440, the menu's Graphics → Resolution; off by default): the game's TV-mode render size, 1920x1080, becomes
//   2560x1440 at the five places it is set (after Fl4sh's "2560x1440" mod for 1.1.1): sharper, and heavier on the GPU. Handheld mode keeps its own 720p.
//   Each word is checked first (MOV / MOVK of 0x780 or 0x438 into the register the game uses); a build or code that differs is left alone
static int arc_res(module_t *m, int64_t p0, int64_t p1) {
    // DM_ARC_RES (Graphics → Resolution): 720 / 1080 (the game's own) / 1440, the TV-mode render size at the five places it is set, width and height
    //   (MOV / MOVK of 0x780 and 0x438). 1440p grows the render-target pool and the GFX2 heap with it (arc_gpu_pool: the start crash of v0.40 came from a pool sized
    //   for 1080p; S25+ 2026-11-20: 2560x1440 at a steady 30). 720p needs no more memory. Handheld mode keeps its own 720p; DM_ARC_RES_FORCE no longer needed
    const char *e = getenv("DM_ARC_RES"); int res = e ? atoi(e) : 0; if (res != 1440 && res != 720) return 0;
    uint32_t W = res == 1440 ? 0xa00 : 0x500, H = res == 1440 ? 0x5a0 : 0x2d0;
    static const struct { uint64_t off; uint32_t orig; int h; } R[] = {
        { 0x29f0650, 0x5280f008u, 0 }, { 0x29f0654, 0x52808709u, 1 }, { 0x29f0410, 0x5280f008u, 0 }, { 0x29f0414, 0x52808709u, 1 },
        { 0x2b9a5fc, 0x5280f008u, 0 }, { 0x2b9a604, 0x52808708u, 1 }, { 0x277bd44, 0x5280f009u, 0 }, { 0x277bd50, 0x52808708u, 1 },
        { 0x29e99e8, 0xf2c0f008u, 0 }, { 0x29e99f4, 0xf2c08708u, 1 } };
    for (unsigned i = 0; i < sizeof R / sizeof *R; i++) { uint32_t w; memcpy(&w, m->base + R[i].off, 4);
        if (R[i].off + 4 > m->text_end || w != R[i].orig || overlaps_rewrite(m, R[i].off, 4, p0, p1)) { LOG("[RED] %s: %dp not applied (Legends: Arceus 1.1.1's render size code differs at +%#llx)", m->name, res, (unsigned long long)R[i].off); return 0; } }
    for (unsigned i = 0; i < sizeof R / sizeof *R; i++) { uint32_t w = (R[i].orig & ~(0xffffu << 5)) | (R[i].h ? H : W) << 5; memcpy(m->base + R[i].off, &w, 4); }
    int g = res == 1440 ? arc_gpu_pool(m, 400, p0, p1) : 1;
    LOG("%s: resolution: the TV-mode render size is %ux%u (DM_ARC_RES)%s", m->name, W, H, res != 1440 ? "" : g ? "; render-target pool 400 MB (was 250), GFX2 heap +150 MB" : "; [RED] the GPU memory sizes differ: left as they are");
    return (int)(sizeof R / sizeof *R);
}
// True aspect ratio for Legends: Arceus 1.1.1 (experimental, DM_WIDE_RENDER=1 with Widescreen): the game renders at the screen's own shape instead of 16:9 stretched to
//   it. The render widths arc_res moves for 1440p: the five TV-mode 1920s (MOV W8 / W9, #0x780, and the MOVK X8, #0x780, LSL #32 of the pair at +0x29e99e4) become
//   1080 x the screen's aspect, and that pair's handheld 1280 (MOVZ X8, #0x500) 720 x it (even, at most 21.6:9); the heights stay. With Widescreen's cameras (arc_wide)
//   the picture keeps square pixels; the HUD is narrowed to 16:9 there already. Off by default: render targets grow ~25 % at 20:9 and the game's GPU memory areas are
//   sized for 1080p (1440p, +78 %, could not start; crash #3's retry may or may not cover the smaller growth). A build or code that differs (1440p applied too) is left alone
static int arc_wide_render(module_t *m, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_WIDE_RENDER"), *a = getenv("DM_ASPECT"), *r = getenv("DM_WIDE_RATIO");
    if (!e || atoi(e) <= 0 || !a || strcmp(a, "wide")) return 0;
    double ratio = r ? atof(r) : 0; if (ratio < 16.0 / 9.0 + 0.01) return 0;
    if (ratio > 2.4) ratio = 2.4;
    uint32_t w1080 = (uint32_t)(1080 * ratio / 2 + 0.5) * 2, w720 = (uint32_t)(720 * ratio / 2 + 0.5) * 2;
    static const struct { uint64_t off; uint32_t orig; int tv; } S[] = {
        { 0x29f0650, 0x5280f008u, 1 }, { 0x29f0410, 0x5280f008u, 1 }, { 0x2b9a5fc, 0x5280f008u, 1 }, { 0x277bd44, 0x5280f009u, 1 }, { 0x29e99e8, 0xf2c0f008u, 1 }, { 0x29e99e4, 0xd280a008u, 0 } };
    for (unsigned i = 0; i < sizeof S / sizeof *S; i++) { uint32_t w; memcpy(&w, m->base + S[i].off, 4);
        if (S[i].off + 4 > m->text_end || w != S[i].orig || overlaps_rewrite(m, S[i].off, 4, p0, p1)) { LOG("[RED] %s: true aspect ratio not applied (the render size code differs at +%#llx, or 1440p is on)", m->name, (unsigned long long)S[i].off); return 0; } }
    for (unsigned i = 0; i < sizeof S / sizeof *S; i++) { uint32_t w = (S[i].orig & ~(0xffffu << 5)) | (S[i].tv ? w1080 : w720) << 5; memcpy(m->base + S[i].off, &w, 4); }
    arc_gpu_pool(m, (unsigned)(250.0 * w1080 / 1920.0 + 0.999), p0, p1);   // (the render targets grow with the width)
    LOG("%s: true aspect ratio for Legends: Arceus 1.1.1: the game renders %ux1080 in TV mode, %ux720 handheld (screen %.4f), instead of 16:9 stretched", m->name, w1080, w720, ratio);
    return (int)(sizeof S / sizeof *S);
}
// Crash #3 (Legends: Arceus 1.1.1, on by default; DM_ARC_POOL_RETRY=0 leaves it out): the CPU-mapped buffer allocator's function at main+0x2a8d6ec (vtable main+0x4185e58
//   slot +0x28) looks a block up on its list (bl find main+0x2a919bc at +0x2a8d714, x0 = list, x1 / x2 its arguments). When that fails it flushes the list (main+0x2a92624)
//   but never looks again, then reads [x21 + 0x30] with x21 = 0 at +0x2a8d750: the crash. The call at +0x2a8d714 now goes to added code in the zero gap at +0x38c8600
//   (the same data-area gap as Widescreen's, far past its 36 bytes; that page is kept executable, mod_page_note): it keeps x0 / x1 / x2, looks up, and when that finds
//   nothing flushes and looks again with the same arguments, up to 4 times. Found: the game goes on as after any lookup that worked; still nothing: the old path, as before.
//   (find / flush keep x23, the list, as the calling convention asks.) Every word is checked first
static int arc_pool_retry(module_t *m, uint64_t lim, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_ARC_POOL_RETRY"); if (e && !strcmp(e, "0")) return 0;
    const uint64_t site = 0x2a8d714, cave = 0x38c8600; const uint32_t orig = 0x940010aau, call = 0x9438ebbbu;   // bl find → bl cave
    static const uint32_t code[32] = {
        0xa9bd7bfdu, 0x910003fdu, 0xa90107e0u, 0xf90013e2u,   // stp x29, x30, [sp, #-48]! / mov x29, sp / stp x0, x1, [sp, #16] / str x2, [sp, #32]
        0x97c724ebu, 0xb5000320u,                             // bl find / cbnz x0, done
        0xaa1703e0u, 0x97c72802u, 0xa94107e0u, 0xf94013e2u, 0x97c724e5u, 0xb5000260u,   // 4x: mov x0, x23 / bl flush / ldp x0, x1, [sp, #16] / ldr x2, [sp, #32] / bl find / cbnz x0, done
        0xaa1703e0u, 0x97c727fcu, 0xa94107e0u, 0xf94013e2u, 0x97c724dfu, 0xb50001a0u,
        0xaa1703e0u, 0x97c727f6u, 0xa94107e0u, 0xf94013e2u, 0x97c724d9u, 0xb50000e0u,
        0xaa1703e0u, 0x97c727f0u, 0xa94107e0u, 0xf94013e2u, 0x97c724d3u, 0xb5000020u,
        0xa8c37bfdu, 0xd65f03c0u };                           // done: ldp x29, x30, [sp], #48 / ret
    uint32_t w; int ok = cave + sizeof code <= lim && site + 4 <= m->text_end;
    if (ok) { memcpy(&w, m->base + site, 4); ok = w == orig && !overlaps_rewrite(m, site, 4, p0, p1); }
    for (unsigned i = 0; ok && i < 32; i++) { memcpy(&w, m->base + cave + 4 * i, 4); ok = !w; }
    if (!ok) { LOG("[RED] %s: pool lookup retry (crash #3) not applied: the allocator's code or the gap differs, or was rewritten", m->name); return 0; }
    memcpy(m->base + cave, code, sizeof code); mod_page_note(m, cave, sizeof code);
    memcpy(m->base + site, &call, 4); mod_page_note(m, site, 4);
    LOG("%s: pool lookup retry (crash #3): a failed lookup at +%#llx flushes and looks again, up to 4 times (code at +%#llx)", m->name, (unsigned long long)site, (unsigned long long)cave);
    return 2;
}
// 3D render scale for Legends: Arceus 1.1.1 (DM_RES_CAP 0.5 - 1, the menu's slider; live). The game keeps its 3D buffer's scale in two registry properties,
//   3DBufferWidthScale / 3DBufferHeightScale (keys 0x4aa9ba3dccea94ea / 0x66e7a756352d0d9f), which its own resolution controller sets (levels 1.0 x 1.0, 0.9 x 0.85,
//   0.85 x 0.8, 0.8 x 0.7; main+0xcc3600) along with reset and restore paths. Four places read them back (vtable +0x38 of the registry, 8 reads): the one that resizes
//   when they change (main+0x29b2468, compared with its copies at +0x520 / +0x524), the 3D buffer's size (+0x29fb28c), the upscale factors (+0x29bc3b0) and a second
//   size (+0x2b9f894). Every read's result is multiplied by the slider's value, so all of them agree whatever path set the property and whatever the game's own level:
//   the instruction after each read (a load of the read's handle, sp / x29 relative) becomes BL to a stub in the zero gap (+0x38c8680, after crash #3's code) that
//   points x16 at the read's result slot, does the replaced load, and goes to one shared routine: s0 = [x16] x the value (a word after it), d0 / d1 kept. x16 holds
//   nothing right after a call (the calling convention's scratch). Live: the value's word is rewritten (dm_arc_scale_set); the first reader sees the change and resizes.
//   Every word is checked first (the eight loads, the gap's zeros)
static volatile uint32_t *g_arc_scale_lit;   // the scale's word in the gap (NULL: not applied)
void dm_arc_scale_set(float c) {
    if (!g_arc_scale_lit || !(c >= 0.5f && c <= 1.0f)) return;
    uint32_t b; memcpy(&b, &c, 4); __atomic_store_n(g_arc_scale_lit, b, __ATOMIC_RELEASE);
    LOG("Legends: Arceus 3D render scale %.2f, set while playing", (double)c);
}
// The scale the 3D is drawn at, as far as the host sets it (the slider times the guard's step; the game's own resolution levels come on top): frame generation's
//   work size follows it (nvn_vk.c fg_work_scale)
// (Legends: Arceus's half of dm_scene_scale, hle_resscale.c: 0 where its scale patch is not in, which is every other game)
float dm_arc_scene_scale(void) { if (!g_arc_scale_lit) return 0.0f; uint32_t b = __atomic_load_n(g_arc_scale_lit, __ATOMIC_ACQUIRE); float f; memcpy(&f, &b, 4); return f >= 0.3f && f <= 1.0f ? f : 1.0f; }
static int arc_scale(module_t *m, uint64_t lim, int64_t p0, int64_t p1) {
    static const struct { uint64_t off; uint32_t orig, call; } S[8] = {
        { 0x29b24b0, 0xf9401be0u, 0x943c5874u }, { 0x29b2504, 0xf9401be0u, 0x943c5862u }, { 0x29fb2d0, 0xf94033e0u, 0x943b34f2u }, { 0x29fb324, 0xf94033e0u, 0x943b34e0u },
        { 0x29bc3dc, 0xf85b83a0u, 0x943c30b5u }, { 0x29bc4b4, 0xf85b83a0u, 0x943c3082u }, { 0x2b9f8cc, 0xf94033e0u, 0x9434a37fu }, { 0x2b9f92c, 0xf94033e0u, 0x9434a36au } };
    static const uint32_t code[31] = {
        0x910053f0u, 0xf9401be0u, 0x14000016u,   0x910043f0u, 0xf9401be0u, 0x14000013u,   // add x16, sp, #0x14 / ldr x0, [sp, #0x30] / b shared ...   (one stub a read)
        0x910023f0u, 0xf94033e0u, 0x14000010u,   0x910243f0u, 0xf94033e0u, 0x1400000du,
        0xd10243b0u, 0xf85b83a0u, 0x1400000au,   0xd10243b0u, 0xf85b83a0u, 0x14000007u,   // sub x16, x29, #0x90 / ldur x0, [x29, #-0x48] / b shared ...
        0xd100d3b0u, 0xf94033e0u, 0x14000004u,   0x910113f0u, 0xf94033e0u, 0x14000001u,
        0x6dbf07e0u, 0xbd400200u, 0x1c0000a1u, 0x1e210800u, 0xbd000200u, 0x6cc107e0u, 0xd65f03c0u };   // shared: stp d0, d1 / ldr s0, [x16] / ldr s1, value / fmul / str / ldp / ret
    const uint64_t cave = 0x38c8680, lit = cave + sizeof code;   // (the value's word: +0x38c86fc)
    int ok = lit + 4 <= lim;
    for (unsigned i = 0; ok && i < 8; i++) { uint32_t w; memcpy(&w, m->base + S[i].off, 4); ok = S[i].off + 4 <= m->text_end && w == S[i].orig && !overlaps_rewrite(m, S[i].off, 4, p0, p1); }
    for (unsigned i = 0; ok && i < 32; i++) { uint32_t w; memcpy(&w, m->base + cave + 4 * i, 4); ok = !w; }
    if (!ok) { LOG("[RED] %s: 3D render scale not applied: the property reads or the gap differ, or were rewritten", m->name); return 0; }
    const char *e = getenv("DM_RES_CAP"); float c = e ? (float)atof(e) : 1.0f; if (!(c >= 0.5f && c <= 1.0f)) c = 1.0f;
    memcpy(m->base + cave, code, sizeof code); memcpy(m->base + lit, &c, 4); mod_page_note(m, cave, sizeof code + 4);
    for (unsigned i = 0; i < 8; i++) { memcpy(m->base + S[i].off, &S[i].call, 4); mod_page_note(m, S[i].off, 4); }
    g_arc_scale_lit = (volatile uint32_t *)(void *)(m->base + lit);
    LOG("%s: 3D render scale for Legends: Arceus 1.1.1: %.2f on the game's own (8 reads of 3DBufferWidthScale / HeightScale; the slider changes it live)", m->name, (double)c);
    return 9;
}
// Galar 1.3.2 (Shield; Sword's sites are 0x30 earlier, the same words): which game this main is, and its address shift from Shield's
static int galar_shift(module_t *m, int64_t *d) {
    if (!strncasecmp(m->build_id, "a16802625e7826bf83b6f9708e475b91", 32)) { *d = 0; return 1; }
    if (!strncasecmp(m->build_id, "a3b75bcd3311385aeed67fbeeb79cbb7", 32)) { *d = -0x30; return 2; }
    return 0;
}
static int words_are(module_t *m, int64_t p0, int64_t p1, const uint64_t *off, const uint32_t *orig, int n) {
    for (int i = 0; i < n; i++) { uint32_t w; if (off[i] + 4 > m->text_end) return 0; memcpy(&w, m->base + off[i], 4); if (w != orig[i] || overlaps_rewrite(m, off[i], 4, p0, p1)) return 0; }
    return 1;
}
// Shadows (DM_SHADOWS, Graphics → Shadows) for Sword and Shield 1.3.2. The main scene's shadow target is 1024 x 1024 with 2 layers = 2 cascades (built in main+0xf679d0:
//   MOV W8, #0x400 / MOV X8, #0x400 at +0xf67d38 / +0xf67d40; handheld copies it), and the fallback target a scene builds when it has none (+0xf56108 / +0xf56110).
//   low: 512 x 512 (a quarter of the memory and fill); lowest: also only the near cascade (the cascade-level setter main+0xf52930 always stores 1: CMP W1, #1 / B.LT
//   → MOV W1, #1 / NOP; far shadows go). The layer count stays 2 (a level-2 request must pass the setter's bounds check). Read once, when the targets are made
static int galar_shadow_patch(module_t *m, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_SHADOWS"); int64_t d; const char *g;
    if (!e || (strcmp(e, "low") && strcmp(e, "lowest"))) return 0;
    int gi = galar_shift(m, &d); if (!gi) return 0; g = gi == 1 ? "Shield 1.3.2" : "Sword 1.3.2";
    uint64_t so[4] = { 0xf67d38 + d, 0xf67d40 + d, 0xf56108 + d, 0xf56110 + d }; const uint32_t sw[4] = { 0x321603e8u, 0xd2808008u, 0x321603e8u, 0xd2808008u }, nw[4] = { 0x52804008u, 0xd2804008u, 0x52804008u, 0xd2804008u };
    uint64_t co[2] = { 0xf52930 + d, 0xf52934 + d }; const uint32_t cw[2] = { 0x7100043fu, 0x5400010bu }, cn[2] = { 0x52800021u, 0xd503201fu };
    int lowest = !strcmp(e, "lowest");
    if (!words_are(m, p0, p1, so, sw, 4) || (lowest && !words_are(m, p0, p1, co, cw, 2))) { LOG("[RED] %s: shadows %s not applied (%s's shadow code differs)", m->name, e, g); return 0; }
    for (int i = 0; i < 4; i++) memcpy(m->base + so[i], &nw[i], 4);
    if (lowest) for (int i = 0; i < 2; i++) memcpy(m->base + co[i], &cn[i], 4);
    LOG("%s: shadows %s for %s: shadow maps 512 x 512%s (DM_SHADOWS)", m->name, e, g, lowest ? ", the near cascade only" : "");
    return lowest ? 6 : 4;
}
// Step pin (DM_STEP_PIN=1, a test switch for the stadium-crowd flicker at 60; off by default) for Sword and Shield 1.3.2. The game picks its step each frame from its
//   frame-time table (main+0x203eeb0: 16.7, 33.3, 50, 66.7, 83.3 ms) by an index it derives from its own measured CPU / GPU time (main+0x5df360 / main+0x5df4b0,
//   CSEL W2 ... at +0x5df460 and +0x5df508). Only entry 1 (+0x203eeb8) is the step word the host feeds at 60 (hle_nvn.c vstep_tick); the other entries are the
//   game's own. Pinned, both selects give index 1 (MOV W2, #1): the step is always that word -- 33.3 ms at the game's own 30, the host's real step at 60. Same
//   addresses in Sword and Shield (this part of the code is not shifted)
static int galar_step_pin(module_t *m, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_STEP_PIN"); int64_t d;
    if (!e || strcmp(e, "1")) return 0;
    int gi = galar_shift(m, &d); if (!gi) return 0;
    uint64_t so[2] = { 0x5df460, 0x5df508 }; const uint32_t ow[2] = { 0x1a89c2c2u, 0x1a8bc122u }, nw = 0x52800022u;
    if (!words_are(m, p0, p1, so, ow, 2)) { LOG("[RED] %s: step pin not applied (the step code differs)", m->name); return 0; }
    for (int i = 0; i < 2; i++) memcpy(m->base + so[i], &nw, 4);
    LOG("%s: step pin for %s: the game's step is always the step word (DM_STEP_PIN, test switch)", m->name, gi == 1 ? "Shield 1.3.2" : "Sword 1.3.2");
    return 2;
}
// Draw distance (DM_DRAW_DIST, percent 50-200, Graphics → Draw distance; from the next start) for Sword and Shield 1.3.2: the terrain detail layers (grass and the other
//   instanced field objects, terrain_draw_setting's visible distance and fade bands, main+0xccb000) are shown by a squared camera distance computed once per layer in
//   main+0xccec80 (FADDP V2.2S at +0xcced80, stored and reused by every comparison). That one is scaled by 1 / k^2 in a small cave at the end of the rewriter's code area:
//   FADDP; LDR S31, =1/k^2; FMUL S2, S2, S31; B back (V31 is unused in that function). Trees, NPCs and Pokémon in the wild are RomFS data, not this
static int galar_draw_patch(module_t *m, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_DRAW_DIST"); int pct = e ? atoi(e) : 100; int64_t d;
    if (pct == 100 || pct < 50 || pct > 200) return 0;
    int gi = galar_shift(m, &d); if (!gi) return 0;
    uint64_t site = 0xcced80 + d; const uint32_t orig = 0x2e20d402u;
    if (!words_are(m, p0, p1, &site, &orig, 1)) { LOG("[RED] %s: draw distance not applied (the terrain layer code differs)", m->name); return 0; }
    // the cave: the zero tail of the rewriter's code area (mapped read + execute with it), 64 bytes checked free
    int64_t end = m->patch_off < 0 ? p1 : (int64_t)m->size; static const uint8_t z[64];
    int64_t cave = end - 32;
    if (end - 64 < p0 || memcmp(m->base + end - 64, z, 64) || llabs(cave - (int64_t)site) >= (1ll << 27)) { LOG("[RED] %s: draw distance not applied (no free code space in reach)", m->name); return 0; }
    float k = pct / 100.0f, lit = 1.0f / (k * k); uint32_t c[5];
    c[0] = orig;                                    // FADDP V2.2S, V0.2S, V0.2S
    c[1] = 0x1c00007fu;                             // LDR S31, [PC, #12]
    c[2] = 0x1e3f0842u;                             // FMUL S2, S2, S31
    c[3] = 0x14000000u | (uint32_t)((((int64_t)site + 4) - (cave + 12)) >> 2 & 0x3ffffff);   // B site + 4
    memcpy(&c[4], &lit, 4);
    memcpy(m->base + cave, c, sizeof c);
    uint32_t br = 0x14000000u | (uint32_t)((cave - (int64_t)site) >> 2 & 0x3ffffff); memcpy(m->base + site, &br, 4);
    __builtin___clear_cache((char *)(m->base + cave), (char *)(m->base + cave + 20));
    LOG("%s: draw distance %d %% for %s: grass and field detail layers (DM_DRAW_DIST)", m->name, pct, gi == 1 ? "Shield 1.3.2" : "Sword 1.3.2");
    return 2;
}
// The game's memory pool running dry (Sword and Shield 1.3.2; DM_POOL_DUMP=0 leaves it out). The Dynamax crash (#211 #325 #330 #341) and the load crash at
//   main+0x5a00a8: a gflib3 tlsf_pool_resource (vtable main+0x250dae0) gives no memory, and the caller uses the null. Its allocate (main+0x661260: the pool at
//   obj+0x28, then the parent resource at obj+0x20) returns null without a word: the game's own dump (vtable +0xb8, main+0x6614e0 → do_dump_no_lock
//   main+0x660910: "[gfl::mem] dump <name> begin", one line per block with the source file and line that took it) runs only where a caller asks for it, and
//   the Dynamax path (the file loader main+0x5fac50 allocating a 2.6 MB VFXB effect file through its request's allocator, no null check) does not.
//   So: at the allocate's end (+0x66131c MOV X0, X22, after the pool's lock is let go) a branch to added code in the zero tail of the rewriter's code area
//   (as galar_draw_patch: the 64 bytes before its own): a null result calls dm_gfl_pool_fail (pool, size asked, sp), which logs the pool, the size and the
//   caller, and runs the game's dump on that pool (hle_sdk16.c h_diag_Log lets "[gfl::mem]" lines into the log). The result is returned unchanged. Same
//   words at the same addresses in both games (checked in both mains)
//   Emergency memory (on by default; DM_POOL_EMERGENCY=0 leaves it out, the report stays): the failed allocate is given memory instead of the null, from a
//   reserved area of our own (768 MB of address space; only what is used becomes real memory), whole pages per block, and the pool's deallocate (main+0x661340,
//   the same in both games) first checks whether a block is from that area and gives it back there. On hardware the game's pools are fixed sizes; something on
//   the port fills some of them further (the Dynamax effect, a Pokemon's textures on changing the lead, a 30 KB vector), and every caller uses the null unchecked.
// (dm_bigstack_enter: hle.h, hle_bigstack.c)
#include <pthread.h>
static uint8_t *g_gfl_main; static int g_gfl_test;
#define GFL_EMG_SIZE (768ull << 20)
static uint8_t *g_emg; static size_t g_emg_pg, g_emg_np, g_emg_next, g_emg_live, g_emg_peak_mb; static uint8_t *g_emg_used; static uint32_t *g_emg_run;
static pthread_mutex_t g_emg_mu = PTHREAD_MUTEX_INITIALIZER;
static int emg_init(void) {
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 4096; g_emg_pg = (size_t)pg; g_emg_np = GFL_EMG_SIZE / g_emg_pg;
    size_t al = 2u << 20; uint8_t *m = mmap(NULL, GFL_EMG_SIZE + al, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) { LOG("[RED] memory pool emergency area: no address space (errno %d)", errno); return 0; }
    g_emg = (uint8_t *)(((uintptr_t)m + al - 1) & ~(uintptr_t)(al - 1));   // (2 MB aligned: a block's alignment up to that is a multiple of the page index)
    g_emg_used = calloc(g_emg_np, 1); g_emg_run = calloc(g_emg_np, sizeof *g_emg_run);
    return g_emg_used && g_emg_run;
}
static void *emg_alloc(uint64_t size, uint64_t align, int *got) {
    *got = 0; if (!g_emg || !size || size > GFL_EMG_SIZE / 4) return NULL;
    size_t np = (size_t)((size + g_emg_pg - 1) / g_emg_pg), step = align > g_emg_pg ? (size_t)(align / g_emg_pg) : 1; void *r = NULL;
    pthread_mutex_lock(&g_emg_mu);
    for (int pass = 0; pass < 2 && !r; pass++) {   // next fit from where the last block ended, then once from the start
        size_t i = pass ? 0 : g_emg_next, lim = pass ? g_emg_next + np : g_emg_np; if (lim > g_emg_np) lim = g_emg_np;
        while (!r) {
            i = (i + step - 1) / step * step; if (i + np > lim) break;
            size_t j = 0; while (j < np && !g_emg_used[i + j]) j++;
            if (j == np) { memset(g_emg_used + i, 1, np); g_emg_run[i] = (uint32_t)np; g_emg_next = i + np; g_emg_live += np; r = g_emg + i * g_emg_pg; }
            else i += j + 1;
        }
    }
    size_t mb = g_emg_live * g_emg_pg >> 20; int grew = r && mb >= g_emg_peak_mb + 16; if (grew) g_emg_peak_mb = mb;
    pthread_mutex_unlock(&g_emg_mu);
    if (grew) LOG("[RED] the game's pools now use %zu MB of emergency memory", mb);
    *got = r != NULL;
    return r;
}
void dm_gfl_pool_free(uint8_t *pool, uint8_t *p);
void dm_gfl_pool_free(uint8_t *pool, uint8_t *p) {
    (void)pool; size_t off = (size_t)(p - g_emg), i = off / g_emg_pg;
    pthread_mutex_lock(&g_emg_mu);
    uint32_t np = (off % g_emg_pg == 0 && i < g_emg_np) ? g_emg_run[i] : 0;
    // (the pages go back to the system before they are marked free, still under the lock: another thread could otherwise be given them in between and lose
    //   what it wrote; the address space stays ours)
    if (np) { madvise(p, (size_t)np * g_emg_pg, MADV_DONTNEED); g_emg_run[i] = 0; memset(g_emg_used + i, 0, np); g_emg_live -= np; }
    pthread_mutex_unlock(&g_emg_mu);
    if (!np) LOG("[RED] the game gave back %p in the emergency area, not a block start: left alone", (void *)p);
}
void *dm_gfl_pool_fail(uint8_t *pool, uint64_t size, uint8_t *sp, uint64_t align);
void *dm_gfl_pool_fail(uint8_t *pool, uint64_t size, uint8_t *sp, uint64_t align) {
    if (g_gfl_test) {   // (DM_POOL_DUMP=test: every allocate comes here; the first one of 2 MB or more is reported and dumped as if it had failed, once; its result is kept)
        static int t; if (size < (2u << 20) || __atomic_exchange_n(&t, 1, __ATOMIC_RELAXED)) return NULL;
        LOG("memory pool report (test): pool %p gave %llu bytes, asked from main+%#llx; the game's own dump of it follows", (void *)pool, (unsigned long long)size,
            (unsigned long long)(*(uint64_t *)(sp + 0x38) - (uint64_t)g_gfl_main));
        ((void (*)(void *))(g_gfl_main + 0x6614e0))(pool); LOG("memory pool report (test): dump done"); return NULL; }
    int got; void *r = emg_alloc(size, align, &got);
    static int n; int k = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED); if (k >= 40) return r;
    uint64_t lr = *(uint64_t *)(sp + 0x38);   // (the allocate's own return address, saved by its prologue: stp x29, x30, [sp, #0x30])
    LOG("[RED] the game's memory pool %p (%s) could not give %llu bytes (%.2f MB), asked from main+%#llx: %s%s", (void *)pool,
        *(uint64_t *)(pool + 0x20) ? "its parent too" : "no parent", (unsigned long long)size, size / 1048576.0, (unsigned long long)(lr - (uint64_t)g_gfl_main),
        got ? "given emergency memory" : g_emg ? "the emergency area has no room either, the game gets nothing" : "no emergency memory (DM_POOL_EMERGENCY=0)",
        k < 2 ? "; the game's own dump of the pool follows" : "");
    if (k < 2) ((void (*)(void *))(g_gfl_main + 0x6614e0))(pool);   // (the pool's dump through its own vtable function: takes the pool's lock, let go above)
    return r;
}
static int galar_pool_dump(module_t *m, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_POOL_DUMP"); if (e && !strcmp(e, "0")) return 0;
    int64_t d; if (!galar_shift(m, &d)) return 0; (void)d;   // (the allocator sits at the same address in Sword and Shield)
    static const uint64_t W[5] = { 0x66131c, 0x661320, 0x6614e0, 0x661340, 0x661344 };
    // MOV X0, X22 / LDP X29, X30, [SP, #0x30] / the dump's STP X22, X21, [SP, #-0x30]! / the deallocate's STR X25, [SP, #-0x50]! and STP X24, X23, [SP, #0x10]
    static const uint32_t O[5] = { 0xaa1603e0u, 0xa9437bfdu, 0xa9bd57f6u, 0xf81b0ff9u, 0xa9015ff8u };
    if (!words_are(m, p0, p1, W, O, 5)) { LOG("[RED] %s: memory pool report not applied (the allocator's code differs)", m->name); return 0; }
    int64_t end = m->patch_off < 0 ? p1 : (int64_t)m->size, cave = end - 128, cave2 = end - 224; static const uint8_t z[96];
    if (cave2 < p0 || memcmp(m->base + cave, z, 64) || memcmp(m->base + cave2, z, 96) || llabs(cave2 - (int64_t)W[0]) >= (1ll << 27)) {
        LOG("[RED] %s: memory pool report not applied (no free code space in reach)", m->name); return 0; }
    g_gfl_main = m->base;
    int test = !strcmp(e ? e : "", "test"); const char *ee = getenv("DM_POOL_EMERGENCY");
    int emg = !test && !(ee && !strcmp(ee, "0")) && emg_init();
    uint64_t ent = (uint64_t)(uintptr_t)dm_bigstack_enter;
    // at the allocate's end: a null result goes to dm_gfl_pool_fail (pool, size, sp, alignment), whose answer (emergency memory) is returned; no answer returns
    //   the allocate's own result (test mode: every result passes through and comes back unchanged)
    uint32_t cc[16] = {
        0xaa1603e0u,   // [0]  MOV X0, X22            (the game's own)
        0xb5000140u,   // [1]  CBNZ X0, [11]          memory given: straight back
        0xaa1303e0u,   // [2]  MOV X0, X19            the pool
        0xaa1503e1u,   // [3]  MOV X1, X21            the size asked
        0x910003e2u,   // [4]  MOV X2, SP             (the allocate's frame: its return address at +0x38)
        0xaa1403e3u,   // [5]  MOV X3, X20            the alignment asked
        0x580000d0u,   // [6]  LDR X16, [12]          dm_gfl_pool_fail
        0x580000f1u,   // [7]  LDR X17, [14]          dm_bigstack_enter: on this thread's host stack (a game thread's own can be small)
        0xd63f0220u,   // [8]  BLR X17                (x1..x18 and x30 are free here: the allocate reloads x29 / x30 and returns x0 only)
        0xf100001fu,   // [9]  CMP X0, #0
        0x9a961000u,   // [10] CSEL X0, X0, X22, NE   no answer: the allocate's own result
        0x14000000u | (uint32_t)((((int64_t)W[1]) - (cave + 44)) >> 2 & 0x3ffffff),   // [11] B +0x661320
        0, 0, 0, 0 };  // [12] [14] the two addresses
    uint64_t fn = (uint64_t)(uintptr_t)dm_gfl_pool_fail; memcpy(&cc[12], &fn, 8); memcpy(&cc[14], &ent, 8);
    if (test) cc[1] = 0xd503201fu;   // (test: NOP the CBNZ)
    memcpy(m->base + cave, cc, sizeof cc);
    __builtin___clear_cache((char *)(m->base + cave), (char *)(m->base + cave + sizeof cc));
    if (emg) {   // at the deallocate's start: a block from the emergency area goes back there; anything else on to the game's own code
        uint32_t dc[24] = {
            0x58000210u,   // [0]  LDR X16, [16]          the area's start
            0x58000231u,   // [1]  LDR X17, [18]          its end
            0xeb10003fu,   // [2]  CMP X1, X16            (x1 = the block)
            0x54000143u,   // [3]  B.LO [13]
            0xeb11003fu,   // [4]  CMP X1, X17
            0x54000102u,   // [5]  B.HS [13]
            0xa9bf7bfdu,   // [6]  STP X29, X30, [SP, #-16]!
            0x910003fdu,   // [7]  MOV X29, SP
            0x58000190u,   // [8]  LDR X16, [20]          dm_gfl_pool_free (pool, block)
            0x580001b1u,   // [9]  LDR X17, [22]          dm_bigstack_enter
            0xd63f0220u,   // [10] BLR X17
            0xa8c17bfdu,   // [11] LDP X29, X30, [SP], #16
            0xd65f03c0u,   // [12] RET                    (the deallocate returns nothing)
            O[3],          // [13] STR X25, [SP, #-0x50]!  (the game's own first instruction)
            0x14000000u | (uint32_t)((((int64_t)W[4]) - (cave2 + 56)) >> 2 & 0x3ffffff),   // [14] B +0x661344
            0xd503201fu,   // [15] NOP
            0, 0, 0, 0, 0, 0, 0, 0 };
        uint64_t lo = (uint64_t)(uintptr_t)g_emg, hi = lo + GFL_EMG_SIZE, fr = (uint64_t)(uintptr_t)dm_gfl_pool_free;
        memcpy(&dc[16], &lo, 8); memcpy(&dc[18], &hi, 8); memcpy(&dc[20], &fr, 8); memcpy(&dc[22], &ent, 8);
        memcpy(m->base + cave2, dc, sizeof dc);
        __builtin___clear_cache((char *)(m->base + cave2), (char *)(m->base + cave2 + sizeof dc));
        uint32_t b2 = 0x14000000u | (uint32_t)((cave2 - (int64_t)W[3]) >> 2 & 0x3ffffff); memcpy(m->base + W[3], &b2, 4);
        __builtin___clear_cache((char *)(m->base + W[3]), (char *)(m->base + W[3] + 4));
    }
    uint32_t br = 0x14000000u | (uint32_t)((cave - (int64_t)W[0]) >> 2 & 0x3ffffff); memcpy(m->base + W[0], &br, 4);
    __builtin___clear_cache((char *)(m->base + W[0]), (char *)(m->base + W[0] + 4));
    LOG("%s: memory pool report%s: when one of the game's memory pools gives nothing, the log names it and the game's own dump of it follows%s (DM_POOL_DUMP%s)", m->name,
        emg ? " and emergency memory" : "", emg ? "; the game gets memory from a reserved area instead of nothing (DM_POOL_EMERGENCY=0 leaves that out)" : "",
        test ? "=test: the first allocate of 2 MB or more is dumped once" : "");
    return 2 + emg;
}
static int builtin_patches(module_t *m, uint64_t lim, int64_t p0, int64_t p1, int modded) {
    (void)modded;
    if (strcmp(m->name, "main")) return 0;
    int n = bmod_apply(m, lim, p0, p1);
    if (pfps_build(m->build_id)) return n + paldea_fps_patch(m, lim, p0, p1);   // (Scarlet / Violet 4.0.0: none of the Sword / Shield patches below are its)
    if (!strncasecmp(m->build_id, "aee8f150dda1b5a838806e1a5ea6827a", 32)) return n + arc_wide(m, lim, p0, p1) + arc_res(m, p0, p1) + arc_wide_render(m, p0, p1) + arc_pool_retry(m, lim, p0, p1) + arc_scale(m, lim, p0, p1);   // (Legends: Arceus 1.1.1: its 60 is hle_nvn.c arc_step, not the Sword / Shield patch)
    n += wide_patch(m, lim, p0, p1);
    n += galar_shadow_patch(m, p0, p1) + galar_draw_patch(m, p0, p1) + galar_step_pin(m, p0, p1);
    n += galar_pool_dump(m, p0, p1);
    n += galar_res_patch(m, p0, p1);   // (before wide_render_patch: with a resolution chosen, that one finds its words changed and stays out)
    n += wide_render_patch(m, p0, p1);
    return n + fps_patch(m, lim, p0, p1);
}
// Built-in: DM_FPS_TARGET=60..120 (opt-in, the in-game menu; experimental) for Pokémon Shield and Sword 1.3.2's main: the present interval the game asks NVN for, 2 (30 fps),
// becomes 1 (in the game's present-interval setter: if the interval asked for differs from the window's, MOV W1, W21 then a call through NVN's set-present-interval
// pointer; → MOV W1, #1), and the frame time the game takes for interval 2 (in a table of u64 ns by interval: 16666667, 33333334, 50000000 ...) becomes 1 / fps.
// From then on presentation paces at that rate and the step word follows the frames actually shown (hle_nvn.c vstep_tick), so game time stays real time when frames
// run late. The two games are the same engine build: Sword's setter is the same 13 instructions as Shield's, 0x90 bytes earlier; the table is at the same place.
// Both words are checked first: another build, or code that differs, is left alone
static const struct { const char *build; uint64_t itv, dt; const char *game; } FPS_SITES[] = {
    { "a16802625e7826bf83b6f9708e475b91", 0x18a2d18, 0x203eeb8, "Shield 1.3.2" },
    { "a3b75bcd3311385aeed67fbeeb79cbb7", 0x18a2c88, 0x203eeb8, "Sword 1.3.2" },
};
// Live frame rate (DM_FPS_LIVE, on unless "0"): with the frame rate at 30, 60 or Auto the patch is applied at 60 Hz slots whatever the target, and the target lives in
//   dm_fps_live (30 or 60), which the in-game menu changes while playing (android_main.c nativeSetting): at 30 every frame is held 2 slots and the game told
//   interval 2, as the game itself does at 30 (hle_nvn.c vstep_tick, reported_hold); at 60 the hold is 1, 2 when frames run late. 90 / 120 (env.txt) stay fixed
int dm_fps_live;   // 0: not live (the game's own 30, or a fixed 90 / 120)
// The game's own 30 where the patch makes the stadium crowd flicker (DM_NATIVE_START, on unless "0"). With the live frame rate the patch is not written at load:
//   the title screen and the opening run exactly as at 30 on the Switch (the game's own present interval 2 and its own step). The crowd's flipbook (the title
//   screen's stadium, and models ob0041..ob0044 stadiumcrowda..d in the opening after the name entry) flickered under the patch even at 30, each frame held 2 slots
//   of 60 Hz by the host with the step fed by it, and not at the game's own 30 (v0.59, which left the patch out at 30). The patch goes in when play reaches a field
//   area (a zone's scripts load, bin/script/amx/z_*.amx) or the menu asks for 60, comes out again while a stadium crowd is loaded (DM_CROWD_30, on unless "0"), and
//   goes back in at the next field area. dm_fps_tick writes the two words on the present thread: the code word through a moment of read + write + execute on its
//   one page (the code area is sealed), the step word in data; while the patch is out the host feeds the step no more (dm_step_word NULL: hle_nvn.c vstep_on)
static struct { uint8_t *itv, *dt; uint32_t wi, wd, owi, owd; int fps, want, on, patched, broken; } g_fp;   // on: the patch is handled here; want: the live rate to go back to
static int g_fp_native = 1;   // the game's own 30 is asked for: from the start until a field area, and while a stadium crowd is loaded
void dm_fps_scene_file(const char *path) {   // hle_fs.c h_fs_OpenFile
    if (!path || !g_fp.on) return;
    int crowd = strstr(path, "_stadiumcrowd") != NULL, zone = !crowd && strstr(path, "/script/amx/z_") != NULL;
    if (!crowd && !zone) return;
    static int c30 = -1; if (c30 < 0) { const char *e = getenv("DM_CROWD_30"); c30 = !(e && !strcmp(e, "0")); }
    if (crowd && c30 && !__atomic_exchange_n(&g_fp_native, 1, __ATOMIC_RELAXED)) LOG("frame rate: the stadium crowd is loading (%s): the game's own 30 until a field area", path);
    if (zone && __atomic_exchange_n(&g_fp_native, 0, __ATOMIC_RELAXED)) LOG("frame rate: a field area loads (%s)", path);
}
static int fp_write(int patched) {
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 4096;
    uint8_t *page = (uint8_t *)((uintptr_t)g_fp.itv & ~(uintptr_t)(pg - 1));
    if (mprotect(page, (size_t)pg, PROT_READ | PROT_WRITE | PROT_EXEC)) {   // (executable throughout: another thread may be running code on this page)
        g_fp.broken = 1; LOG("[RED] frame rate: the 60 fps patch cannot be %s (mprotect: %s): it stays as it is for this run", patched ? "written" : "taken out", strerror(errno)); return 0; }
    __atomic_store_n((uint32_t *)g_fp.itv, patched ? g_fp.wi : g_fp.owi, __ATOMIC_RELEASE);
    mprotect(page, (size_t)pg, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)g_fp.itv, (char *)g_fp.itv + 4);
    if (!patched) dm_step_word = NULL;   // (the host stops feeding the step before the game's own value goes back)
    dm_guest_write_prepare(g_fp.dt, 4); __atomic_store_n((uint32_t *)g_fp.dt, patched ? g_fp.wd : g_fp.owd, __ATOMIC_RELAXED);
    if (patched) { dm_step_hz = g_fp.fps; dm_step_word = g_fp.dt; }
    g_fp.patched = patched; return 1;
}
// Each present (hle_nvn.c, the present thread, before the step is decided): the patch in or out as asked
void dm_fps_tick(void) {
    if (!g_fp.on || g_fp.broken) return;
    int live = __atomic_load_n(&dm_fps_live, __ATOMIC_RELAXED), nat = __atomic_load_n(&g_fp_native, __ATOMIC_RELAXED);
    if (!g_fp.patched) {
        if (live == 60) { nat = 0; __atomic_store_n(&g_fp_native, 0, __ATOMIC_RELAXED); g_fp.want = 60; }   // (the menu asked for 60 meanwhile: its choice)
        if (!nat && fp_write(1)) { __atomic_store_n(&dm_fps_live, g_fp.want, __ATOMIC_RELAXED);
            LOG("frame rate: the 60 fps patch is in, live frame rate %d (the menu switches 30 / 60 at once)", g_fp.want); }
    } else if (nat) {
        g_fp.want = live == 60 ? 60 : 30;
        if (fp_write(0)) { __atomic_store_n(&dm_fps_live, 30, __ATOMIC_RELAXED); LOG("frame rate: the 60 fps patch is out: the game's own 30 (then %d again)", g_fp.want); }
    }
}
static int fps_patch(module_t *m, uint64_t lim, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_FPS_TARGET"), *lv = getenv("DM_FPS_LIVE"); int live = !(lv && !strcmp(lv, "0"));
    int want = e && *e ? atoi(e) : 0;
    if (!want) { const char *a = getenv("DM_AUTO_FPS"); want = a && atoi(a) >= 60 ? 60 : 30; }   // (Frame rate on Auto: the phone profile's)
    int fps = want > 60 && want <= 120 ? want : want == 60 || (live && want == 30) ? 60 : 0;
    if (!fps) return 0;
    int si = -1; for (unsigned i = 0; i < sizeof FPS_SITES / sizeof *FPS_SITES; i++) if (!strncasecmp(m->build_id, FPS_SITES[i].build, 32)) si = (int)i;
    if (si < 0) { if (want >= 60) LOG("[RED] %s: %d fps patch not applied (not Pokémon Sword / Shield 1.3.2's main)", m->name, fps); return 0; }   // (at 30: nothing was asked for)
    const uint64_t itv = FPS_SITES[si].itv, dt = FPS_SITES[si].dt;
    // (the step word is rewritten every frame, so it must lie past the code area, which is sealed read + execute)
    if (itv + 4 > m->text_end || dt < m->text_end || dt + 4 > lim || overlaps_rewrite(m, itv, 4, p0, p1)) {
        LOG("[RED] %s: %d fps patch not applied (%s: the step word lies in code, or that code was rewritten)", m->name, fps, FPS_SITES[si].game); return 0; }
    uint32_t wi, wd; memcpy(&wi, m->base + itv, 4); memcpy(&wd, m->base + dt, 4);
    if ((wi != 0x2a1503e1u && wi != 0x52800041u) || wd != 33333334u) { LOG("[RED] %s: %d fps patch not applied (unexpected words %08x %u)", m->name, fps, wi, wd); return 0; }
    wi = 0x52800021u; wd = (uint32_t)((1000000000ull + (uint64_t)fps - 1) / (uint64_t)fps);
    { const char *ns = getenv("DM_NATIVE_START");
      if (fps == 60 && live && !(ns && !strcmp(ns, "0"))) {   // the game's own 30 until a field area or 60 from the menu (dm_fps_tick)
        g_fp.itv = m->base + itv; g_fp.dt = m->base + dt; g_fp.wi = wi; g_fp.wd = wd; g_fp.fps = fps; g_fp.want = want == 60 ? 60 : 30; g_fp.on = 1;
        memcpy(&g_fp.owi, m->base + itv, 4); g_fp.owd = 33333334u;   // (the game's own words: the patch can come out again)
        __atomic_store_n(&dm_fps_live, 30, __ATOMIC_RELAXED);
        LOG("%s: 60 fps patch for %s ready, not written yet: the game's own 30 until play reaches a field area or the menu asks for 60, then %d (live frame rate)",
            m->name, FPS_SITES[si].game, g_fp.want);
        return 2; } }
    memcpy(m->base + itv, &wi, 4); memcpy(m->base + dt, &wd, 4);
    dm_step_hz = fps; dm_step_word = m->base + dt;
    if (fps == 60 && live) __atomic_store_n(&dm_fps_live, want == 60 ? 60 : 30, __ATOMIC_RELAXED);
    LOG("%s: %d fps patch for %s (present interval 1 at +%#llx, step %.2f ms at +%#llx, then paced by the host)%s", m->name, fps, FPS_SITES[si].game, (unsigned long long)itv, wd / 1e6,
        (unsigned long long)dt, dm_fps_live ? (want == 60 ? "; live frame rate, now 60 (the menu switches 30 / 60 at once)" : "; live frame rate, now 30 (the menu switches 30 / 60 at once)") : "");
    return 2;
}
static uint8_t *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open %s", path);
    struct stat st; fstat(fd, &st);
    uint8_t *p = malloc(st.st_size);
    if (read(fd, p, st.st_size) != st.st_size) die("read %s", path);
    close(fd); *n = st.st_size; return p;
}

// Seal the code area immediately after copying into anonymous pages (16 KB pages): [0, text_end) and the patch area are read-only+execute, and from then on any write to the code area crashes on the spot;
// what lies between (ro and data on the same page can only go together) is writable
static void map_image(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    size_t sz; uint8_t *raw = slurp(path, &sz);
    if (m->patch_off < 0) {
        // Patch area before the base: file = the first part of the image (text + ro + data) + patch area; bss and the key page are anonymous zero pages. The gap from the patch area's end to the base is left inaccessible (PROT_NONE)
        uint64_t pre = (uint64_t)-m->patch_off, fl = m->file_image_len;
        if (sz < fl) die("%s is shorter than FILE_IMAGE_LEN %#llx", path, (unsigned long long)fl);
        m->size = m->image_size; m->patch_len = (sz - fl + 0x3FFF) & ~0x3FFFull;
        if (m->patch_len > pre) die("%s patch area of %#llx bytes does not fit in the %#llx before the base", path, (unsigned long long)m->patch_len, (unsigned long long)pre);
        uint8_t *a = mmap(NULL, pre + m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (a == MAP_FAILED) die("%s mmap %#llx", m->name, (unsigned long long)(pre + m->size));
        m->base = a + pre;
        memcpy(m->base, raw, fl); memcpy(a, raw + fl, sz - fl); free(raw);
        int modded = pchtxt_dir(m, fl, m->patch_off, m->patch_off + (int64_t)m->patch_len); modded += builtin_patches(m, fl, m->patch_off, m->patch_off + (int64_t)m->patch_len, modded);
        install_key_page(m);
        uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
        if (mprotect(m->base, rx, PROT_READ | PROT_EXEC)) die("%s mprotect text", m->name);
        if (modded) __builtin___clear_cache((char *)m->base, (char *)(m->base + rx));
        mod_pages_open(m, rx);
        if (mprotect(a, m->patch_len, PROT_READ | PROT_EXEC)) die("%s mprotect patch", m->name);
        if (pre > m->patch_len && mprotect(a + m->patch_len, pre - m->patch_len, PROT_NONE)) die("%s mprotect gap", m->name);
        return;
    }
    m->size = (sz + 0x3FFF) & ~0x3FFFull;
    m->base = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m->base == MAP_FAILED) die("%s mmap", m->name);
    memcpy(m->base, raw, sz); free(raw);
    int modded = pchtxt_dir(m, m->key_page_off, m->patch_off, (int64_t)sz); modded += builtin_patches(m, m->key_page_off, m->patch_off, (int64_t)sz, modded);
    install_key_page(m);
    uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
    if (mprotect(m->base, rx, PROT_READ | PROT_EXEC)) die("%s mprotect text", m->name);
    if (modded) __builtin___clear_cache((char *)m->base, (char *)(m->base + rx));
    mod_pages_open(m, rx);
    uint64_t po = m->patch_off & ~0x3FFFull;
    if (mprotect(m->base + po, m->size - po, PROT_READ | PROT_EXEC)) die("%s mprotect patch", m->name);
}
#endif

void load_modules(const char *dir, const char **names, int n) {
    // Map everything and parse the symbol tables first, then relocate all at once (cross-module symbols must all be in place first)
    // DM_BOOTPROF: breakdown of ① host loading (milliseconds), to set the upper bound for ③ pre-relocation / mmap
    uint64_t t0 = now_ns(), tm = 0, tp = 0, tg = 0, tr = 0, t;
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        strlcpy(m->name, names[i], sizeof m->name);
        read_layout(m, dir);
    }
    create_keys();
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        t = now_ns(); map_image(m, dir); tm += now_ns() - t;   // On return the code area is already read-only+execute; from here on the loader only writes data
        if (!i) tsd_check();
        t = now_ns(); parse_dynamic(m); tp += now_ns() - t;
        t = now_ns(); register_gates(m, dir); tg += now_ns() - t;
        LOG("%s: base %p size %#zx", names[i], m->base, m->size);
        g_nmod++;
    }
    force_init();
    t = now_ns(); for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]); tr = now_ns() - t;
    force_check();
    { void hlecount_seal(void); hlecount_seal(); }   // DM_HLECOUNT: relocation done, seal the counting stub pages as read-only executable
    if (getenv("DM_SYMCHECK")) LOG("DM_SYMCHECK: %lu symbol lookups at load time, hash and linear disagree %lu", g_symchk_n, g_symchk_bad);
    if (g_flag.bootprof) LOG("① breakdown: mapping (read + copy + seal pages) %.1f ms, dynamic section parsing %.1f ms, gate registration %.1f ms, relocation + forced takeover %.1f ms, total %.1f ms",
                             tm / 1e6, tp / 1e6, tg / 1e6, tr / 1e6, (now_ns() - t0) / 1e6);
}

// Plays the role of rtld's InitModules (rtld+0x830): call each module's DT_INIT in forward order of the load list.
// The timing is decided by nn::init::Start: after nninitStartup and _init_libc2, before nnMain (the 4th argument callback of Start).
void run_module_inits(void) {
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!m->init) continue;
        LOG("%s: DT_INIT +%#llx (init_array %llu entries)", m->name, m->init, m->init_arraysz / 8);
        ((void (*)(void))(m->base + m->init))();
    }
}
#if defined(__ANDROID__) || defined(__linux__)
#include <sys/uio.h>
// Frame generation's camera guidance (nvn_vk.c fg_cam_*): Legends: Arceus 1.1.1's main camera, read at present time. The camera registry R = *(main+0x42ef5a8);
//   the game's own lookup (main+0x2a11e64, a libc++ unordered_map: buckets R+0x98, count R+0xa0, node {next, hash, key, ..., int index at +0x30}) finds "Main"
//   (key 0x6723799257d05608); the camera = *(*(R+0x60) + index x 0x20), live while its byte +0xd0 is set; view (world → view) at +0x50 and projection at +0x90,
//   4x4 column-major (main+0x29fb138: the scene's constant buffer takes them from there). Each read through process_vm_readv: a pointer that is not (yet) valid
//   while a scene loads fails the read instead of faulting. 1: vp = projection x view
static int peek(uint64_t a, void *o, size_t n) { struct iovec l = { o, n }, r = { (void *)(uintptr_t)a, n }; return a && process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n; }
// Sword and Shield 1.3.2 (the same data and struct offsets in both): the graphics system S = *(main+0x26365b8) (hle_resscale's global), its render-pipeline holder
//   *(S+0x88), the render manager *(holder+0x58), the main 3D render view *(manager+0x160) (the game's own GetMainRenderView, Shield main+0xf67310 / Sword +0xf672e0),
//   and that view's camera object V = *(view+0x128), which main+0x607670 (the Widescreen patch's function) rebuilds each update from the active camera: view at
//   V+0x60, projection +0xa0, view-projection +0xe0, 4x4 column-major, GL form. A perspective camera has projection col2.w = -1 and col3.w = 0 (an orthographic one
//   is skipped), the view's bottom row (0,0,0,1)
static int galar_cam_viewproj(uint8_t *base, float vp[16]) {
    uint64_t S, X, M, RV, V; float m[48];
    if (!peek((uint64_t)(uintptr_t)(base + 0x26365b8), &S, 8) || !peek(S + 0x88, &X, 8) || !peek(X + 0x58, &M, 8) || !peek(M + 0x160, &RV, 8) || !peek(RV + 0x128, &V, 8)) return 0;
    if (!peek(V + 0x60, m, sizeof m)) return 0;   // view, projection, view-projection
    for (int i = 0; i < 48; i++) if (!__builtin_isfinite(m[i])) return 0;
    if (m[16 + 11] != -1.0f || m[16 + 15] != 0.0f) return 0;                                  // perspective only
    if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f || m[15] != 1.0f) return 0;            // a rigid view
    for (int c = 0; c < 3; c++) { float l = m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]; if (l < 0.998f || l > 1.002f) return 0; }
    memcpy(vp, m + 32, 64);
    return 1;
}
int dm_cam_viewproj(float vp[16]) {
    static uint8_t *base, *galar; static int tried;
    if (!tried) { tried = 1; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        if (!strncasecmp(g_mod[i].build_id, "aee8f150dda1b5a838806e1a5ea6827a", 32)) base = g_mod[i].base;
        else if (!strncasecmp(g_mod[i].build_id, "a16802625e7826bf83b6f9708e475b91", 32) || !strncasecmp(g_mod[i].build_id, "a3b75bcd3311385aeed67fbeeb79cbb7", 32)) galar = g_mod[i].base; } }
    if (galar) return galar_cam_viewproj(galar, vp);
    if (!base) return 0;
    const uint64_t key = 0x6723799257d05608ull; uint64_t R, n, bk, node;
    if (!peek((uint64_t)(uintptr_t)(base + 0x42ef5a8), &R, 8) || !peek(R + 0xa0, &n, 8) || !n || n > (1u << 20)) return 0;
    uint64_t idx = __builtin_popcountll(n) <= 1 ? (key & (n - 1)) : key % n;
    if (!peek(R + 0x98, &bk, 8) || !peek(bk + idx * 8, &node, 8) || !node || !peek(node, &node, 8)) return 0;
    for (int guard = 0; node && guard < 64; guard++) {
        uint64_t h[2]; if (!peek(node + 8, h, 16)) return 0;
        if (h[0] == key && h[1] == key) break;
        uint64_t hb = __builtin_popcountll(n) <= 1 ? (h[0] & (n - 1)) : h[0] % n; if (hb != idx) return 0;
        if (!peek(node, &node, 8)) return 0;
    }
    int32_t ci; uint64_t vec, cam; uint8_t live; float v[16], pj[16];
    if (!node || !peek(node + 0x30, &ci, 4) || ci < 0 || !peek(R + 0x60, &vec, 8) || !peek(vec + (uint64_t)ci * 0x20, &cam, 8) || !peek(cam + 0xd0, &live, 1) || !live) return 0;
    if (!peek(cam + 0x50, v, sizeof v) || !peek(cam + 0x90, pj, sizeof pj)) return 0;
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) { float t = 0; for (int k = 0; k < 4; k++) t += pj[k * 4 + r] * v[c * 4 + k]; vp[c * 4 + r] = t; }
    for (int i = 0; i < 16; i++) if (vp[i] != vp[i]) return 0;
    return 1;
}
#endif
