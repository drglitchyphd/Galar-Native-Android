// pso_guard.h on the PC: the pipeline-in-progress marker and the skip list it feeds (nvn_vk.c pg_init / pg_begin / pg_end / nvn_vk_pso_crash_note / pg_skipped).
//   The marker file is written the way nvn_vk.c writes it (a slot per thread at slot * 64 with pwrite, the crash handler's 4-byte "crashed" at offsetof), then read
//   back and taken into the list as pg_init does: a crashed build is skipped at once, an unmarked one only when the next run leaves it in progress again, another
//   driver's slots count for nothing, and another driver's list starts over.
// usage: make t_pso_guard && ./t_pso_guard
#include "pso_guard.h"
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static char g_dir[256], g_mark[300], g_list[300];
static void key(uint8_t k[8], int v) { for (int i = 0; i < 8; i++) k[i] = (uint8_t)(v * 31 + i); }
// one run's builds: slot i building (kind, a, b), and whether the crash handler marked it (as nvn_vk.c: pwrite of the slot, then of 4 bytes at offsetof crashed)
static void run_marks(const int (*b)[4], int n, uint64_t ident) {
    int fd = open(g_mark, O_RDWR | O_CREAT | O_TRUNC, 0644); if (ftruncate(fd, (off_t)sizeof(pg_slot) * PG_SLOTS)) {}
    for (int i = 0; i < n; i++) { pg_slot s; memset(&s, 0, sizeof s); s.magic = PG_MAGIC; s.kind = (uint32_t)b[i][0]; s.ident = ident; s.tid = 1000 + i;
        key(s.k0, b[i][1]); if (b[i][0] == PG_GRAPHICS) key(s.k1, b[i][2]);
        if (pwrite(fd, &s, sizeof s, (off_t)i * 64) != 64) printf("pwrite failed\n");
        if (b[i][3]) { uint32_t one = 1; if (pwrite(fd, &one, 4, (off_t)i * 64 + (off_t)offsetof(pg_slot, crashed)) != 4) printf("pwrite failed\n"); } }
    // a build that finished: its slot cleared (pg_end: the magic word only)
    { pg_slot s; memset(&s, 0, sizeof s); s.magic = PG_MAGIC; s.kind = PG_GRAPHICS; s.ident = ident; key(s.k0, 99); key(s.k1, 98);
      if (pwrite(fd, &s, sizeof s, (off_t)(PG_SLOTS - 1) * 64) != 64) printf("pwrite failed\n");
      uint32_t z = 0; if (pwrite(fd, &z, 4, (off_t)(PG_SLOTS - 1) * 64) != 4) printf("pwrite failed\n"); }
    close(fd);
}
// the next start (pg_init): the marker read, the list loaded and taken into, saved; returns pg_load's answer
static int start(uint64_t ident, pg_list *L, int *added, int *sus) {
    pg_slot old[PG_SLOTS]; memset(old, 0, sizeof old);
    int fd = open(g_mark, O_RDONLY); if (fd >= 0) { if (pread(fd, old, sizeof old, 0) < 0) memset(old, 0, sizeof old); close(fd); }
    int had = pg_load(g_list, ident, L); if (had < 0) unlink(g_list);
    *added = pg_take(L, old, PG_SLOTS, sus);
    if (L->n || L->ns) pg_save(g_list, L); else unlink(g_list);
    unlink(g_mark);
    return had;
}
static int has(const pg_list *L, int kind, int a, int b) { uint8_t k0[8], k1[8], k[16]; key(k0, a); key(k1, b); pg_keys(k0, kind == PG_GRAPHICS ? k1 : NULL, k); return pg_find(L->e, L->n, kind, k) >= 0; }
int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/t_pso_guard_%d", (int)getpid()); char cmd[400]; snprintf(cmd, sizeof cmd, "mkdir -p %s", g_dir); if (system(cmd)) return 1;
    snprintf(g_mark, sizeof g_mark, "%s/pso_inflight", g_dir); snprintf(g_list, sizeof g_list, "%s/pso_skip.txt", g_dir);
    CHECK(sizeof(pg_slot) == 64 && offsetof(pg_slot, crashed) == 20, "a slot is 64 bytes, crashed at offset 20 (the crash handler's pwrite)");
    // the driver as one number: another version, name, ID or info is another driver; the same is the same
    uint64_t q = pg_ident(0x5143, 8, 0x80267002, "Adreno (TM) 730", "0615.87.2"), q2 = pg_ident(0x5143, 8, 0x80267002, "Adreno (TM) 730", "0615.87.2");
    uint64_t tu = pg_ident(0x5143, 18, 0x6802063, "Turnip Adreno (TM) 725", "Mesa 25.3");
    CHECK(q == q2 && q != tu && q != pg_ident(0x5143, 8, 0x80267003, "Adreno (TM) 730", "0615.87.2") && q != pg_ident(0x5143, 8, 0x80267002, "Adreno (TM) 730", "0615.87.3")
          && q != pg_ident(0x5143, -1, 0x80267002, "Adreno (TM) 730", "0615.87.2"), "pg_ident: one number per driver");
    pg_list L; int added, sus, had;
    // 1. a clean first start: nothing
    unlink(g_list); unlink(g_mark);
    had = start(q, &L, &added, &sus); CHECK(had == 0 && L.n == 0 && L.ns == 0 && added == 0, "no files: an empty list (had %d n %d)", had, L.n);
    // 2. the Fold4's crash: the prewarm thread crashed building vertex 5b89... / fragment 7941... (marked), another thread was building another one (not marked)
    { const int b[][4] = { { PG_GRAPHICS, 14, 15, 1 }, { PG_GRAPHICS, 13, 12, 0 } }; run_marks(b, 2, q); }
    had = start(q, &L, &added, &sus);
    CHECK(added == 1 && L.n == 1 && has(&L, PG_GRAPHICS, 14, 15) && !has(&L, PG_GRAPHICS, 13, 12) && L.ns == 1 && sus == 1,
          "a marked slot is skipped at once, the other thread's build is only a suspect (added %d n %d ns %d)", added, L.n, L.ns);
    CHECK(!has(&L, PG_GRAPHICS, 99, 98), "a finished build (its slot cleared) is nothing");
    CHECK(!has(&L, PG_COMPUTE, 14, 0) && !has(&L, PG_GRAPHICS, 15, 14), "the kind and the key order count");
    // 3. the next start: the list read back from its file
    { pg_list R; CHECK(pg_load(g_list, q, &R) == 1 && R.n == 1 && R.ns == 1 && has(&R, PG_GRAPHICS, 14, 15), "pso_skip.txt read back (n %d ns %d)", R.n, R.ns); }
    // 4. the suspect left in progress again (an abort the crash handler does not see, twice): skipped too; a run that crashes elsewhere leaves nothing new
    { const int b[][4] = { { PG_GRAPHICS, 13, 12, 0 } }; run_marks(b, 1, q); }
    had = start(q, &L, &added, &sus);
    CHECK(had == 1 && added == 1 && L.n == 2 && has(&L, PG_GRAPHICS, 13, 12) && L.ns == 0, "a suspect left in progress by the next run too is skipped (added %d n %d ns %d)", added, L.n, L.ns);
    // 5. a closed app: a suspect once, forgotten when the next run does not leave it again
    { const int b[][4] = { { PG_GRAPHICS, 40, 41, 0 } }; run_marks(b, 1, q); }
    had = start(q, &L, &added, &sus); CHECK(added == 0 && L.ns == 1 && L.n == 2, "a build left once: a suspect");
    had = start(q, &L, &added, &sus); CHECK(added == 0 && L.ns == 0 && L.n == 2 && !has(&L, PG_GRAPHICS, 40, 41), "a suspect the next run did not leave again is forgotten");
    // 6. compute pipelines (prewarm_compute, dispatch): their own kind
    { const int b[][4] = { { PG_COMPUTE, 7, 0, 1 } }; run_marks(b, 1, q); }
    had = start(q, &L, &added, &sus); CHECK(added == 1 && has(&L, PG_COMPUTE, 7, 0) && !has(&L, PG_GRAPHICS, 7, 0), "a crashed compute build is skipped as a compute one");
    // 7. a slot of another driver (the marker of a run on the phone's own driver read at a start on Turnip) counts for nothing
    { const int b[][4] = { { PG_GRAPHICS, 50, 51, 1 } }; run_marks(b, 1, tu); }
    had = start(q, &L, &added, &sus); CHECK(added == 0 && !has(&L, PG_GRAPHICS, 50, 51) && L.n == 3, "another driver's slot is not counted");
    // 8. the driver changed: the list starts over (pg_load -1, the file deleted)
    had = start(tu, &L, &added, &sus); CHECK(had == -1 && L.n == 0 && access(g_list, F_OK) != 0, "another driver: the list starts over and its file goes (had %d n %d)", had, L.n);
    // 9. a damaged file: bad lines skipped, the good one kept; a file without the driver line counts as another driver's
    { FILE *f = fopen(g_list, "w"); fprintf(f, "driver %016llx\nskip graphics 0011\nnonsense\nskip graphics zz00000000000000 0000000000000000\n", (unsigned long long)q);
      char a[17], b[17]; uint8_t k0[8], k1[8]; key(k0, 3); key(k1, 4); pg_hex(k0, 8, a); pg_hex(k1, 8, b); fprintf(f, "skip graphics %s %s\n", a, b); fclose(f); }
    { pg_list R; CHECK(pg_load(g_list, q, &R) == 1 && R.n == 1 && has(&R, PG_GRAPHICS, 3, 4), "a damaged list: the good line kept (n %d)", R.n); }
    { FILE *f = fopen(g_list, "w"); fprintf(f, "skip graphics 0000000000000000 0000000000000000\n"); fclose(f); pg_list R; CHECK(pg_load(g_list, q, &R) == -1, "no driver line: started over"); }
    // 10. the list is bounded (PG_MAX): a full list takes no more, and says so with 0
    { pg_list R; memset(&R, 0, sizeof R); uint8_t k[16] = {0}; int ok = 1;
      for (int i = 0; i < PG_MAX; i++) { k[0] = (uint8_t)i; k[1] = (uint8_t)(i >> 8); ok &= pg_add(R.e, &R.n, PG_GRAPHICS, k) == 1; }
      k[0] = 0xff; k[1] = 0xff; CHECK(ok && R.n == PG_MAX && pg_add(R.e, &R.n, PG_GRAPHICS, k) == 0 && R.n == PG_MAX, "the list holds PG_MAX entries, then takes no more"); }
    // 11. save and load keep every entry and suspect, in order
    { pg_list W; memset(&W, 0, sizeof W); W.ident = tu; uint8_t k[16];
      for (int i = 0; i < 20; i++) { memset(k, i, 16); pg_add(i % 3 ? W.e : W.s, i % 3 ? &W.n : &W.ns, i % 2 ? PG_COMPUTE : PG_GRAPHICS, k); }
      pg_list R; CHECK(pg_save(g_list, &W) && pg_load(g_list, tu, &R) == 1 && R.n == W.n && R.ns == W.ns && !memcmp(R.e, W.e, sizeof W.e[0] * (size_t)W.n) && !memcmp(R.s, W.s, sizeof W.s[0] * (size_t)W.ns),
            "save and load: the same entries (n %d / %d, ns %d / %d)", R.n, W.n, R.ns, W.ns); }
    unlink(g_list); unlink(g_mark); rmdir(g_dir);
    printf(fails ? "%d of %d FAILED\n" : "all %d passed\n", fails ? fails : checks, checks);
    return fails != 0;
}
