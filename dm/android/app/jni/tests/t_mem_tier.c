// Memory tier unit test (mem_tier.h): which phones get half textures by default from the memory free at start. The figures are from real logs (the POCO F6 of the
// 2026-11-24 memory analysis: 7,293 MB in all, 1.4-1.7 GB available; "8 GB" Samsungs report 7.42 GB; 12 GB phones about 11.2 M kB). Expected values are hand-written.
// Linux / WSL / macOS: cc -o /tmp/t_mem_tier t_mem_tier.c && /tmp/t_mem_tier
#include "../mem_tier.h"
#include <stdlib.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", s); } while (0)
#define MB(x) ((long)(x) * 1024L)

int main(void) {
    CHECK(mem_tier_half(MB(1600), MB(7293)), "POCO F6, 1.6 GB of 7.3 GB available: half");
    CHECK(mem_tier_half(MB(2559), 7600000), "an 8 GB Samsung (7.6 M kB) with 2,559 MB available: half");
    CHECK(!mem_tier_half(MB(2560), 7600000), "the same with exactly 2.5 GB available: left to the profile");
    CHECK(!mem_tier_half(MB(3400), MB(7293)), "an 8 GB phone with 3.4 GB available: left to the profile");
    CHECK(!mem_tier_half(MB(1500), 11200000), "a 12 GB phone (11.2 M kB) with 1.5 GB available: left to the profile");
    CHECK(!mem_tier_half(MB(1500), 9000000), "exactly 9,000,000 kB in all: left to the profile");
    CHECK(mem_tier_half(MB(900), MB(3800)), "a 4 GB phone with 0.9 GB available: half");
    CHECK(!mem_tier_half(0, MB(7293)), "MemAvailable unknown: nothing decided");
    CHECK(!mem_tier_half(MB(1600), 0), "MemTotal unknown: nothing decided");

    // reading a meminfo file (the kernel's own layout)
    char path[] = "/tmp/t_mem_tier_XXXXXX"; int fd = mkstemp(path); FILE *f = fdopen(fd, "w");
    fputs("MemTotal:        7468032 kB\nMemFree:          301204 kB\nMemAvailable:    1689340 kB\nBuffers:            1024 kB\n", f); fclose(f);
    long av, tot; int ok = mem_tier_read(path, &av, &tot);
    CHECK(ok && av == 1689340 && tot == 7468032, "meminfo read: MemAvailable 1689340 kB, MemTotal 7468032 kB");
    CHECK(mem_tier_half(av, tot), "and that file's phone gets half textures");
    f = fopen(path, "w"); fputs("MemTotal:        7468032 kB\nMemFree:          301204 kB\n", f); fclose(f);   // (kernels before 3.14: no MemAvailable)
    CHECK(!mem_tier_read(path, &av, &tot) && av == 0 && !mem_tier_half(av, tot), "no MemAvailable line: nothing decided");
    unlink(path);
    CHECK(!mem_tier_read("/nonexistent/meminfo", &av, &tot), "no file: nothing decided");

    printf("t_mem_tier: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
