// The load detector (nvn/load_detect.h) on (reads, draws) sequences shaped like the players' logs (fable-loads.md section 0 / L1): Arceus #98's open-world
//   streaming (430-530 draws a frame, 19 MB in bursts over 12 s: the old reads-only rule called it a 12 s load) must not fire; #112's title-to-world load
//   (20-40 draws a frame, about 250 MB in 10 s at about 45 fps) must, and must end at the first full frame. The per-frame numbers are synthetic: the logs keep
//   10 s summaries, not every frame. Plus a menu, a frozen picture and the quiet end. Plain C:  cc -O1 -Wall -I../nvn -o t_load_detect t_load_detect.c && ./t_load_detect
#include "load_detect.h"
#include <stdio.h>
#include <stdlib.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); } while (0)
static uint32_t rnd_s = 12345; static unsigned rnd(unsigned n) { rnd_s = rnd_s * 1103515245u + 12345u; return (rnd_s >> 8) % n; }
#define MS 1000000ull
#define S 1000000000ull

int main(void) {
    {   // #98: 60 fps play for 30 s, 430-530 draws; a 1 s burst of 20 MB/s every 5 s (more than 8 MB/s: the old rule fired on it)
        load_det d = { 0 }; uint64_t b = 0; int fired = 0;
        for (uint64_t t = 16 * MS; t < 30 * S; t += 16667 * 1000ull) {
            if ((t / S) % 5 == 0) b += 20ull * 1048576 * 16667 / 1000000;
            if (load_det_present(&d, t, b, 430 + rnd(100)) > 0) fired++;
            if ((t / (500 * MS)) != ((t - 16667000) / (500 * MS)) && load_det_poll(&d, t, b) > 0) fired++;
        }
        CHECK(!fired, "#98 streaming (bursts of 20 MB/s, 430-530 draws): no load (fired %d times)", fired);
    }
    {   // #112: 5 s of play at 45 fps (500 draws), a 10 s loading screen (20-40 draws, 25 MB/s, the reads pausing 1.5 s at 6 s), a 1 s fade-in without reads at
        //   40 draws, then the world (600 draws)
        load_det d = { 0 }; uint64_t b = 0, on_at = 0, off_at = 0, first_full = 0; int ons = 0, offs = 0;
        for (uint64_t t = 22 * MS; t < 20 * S; t += 22222 * 1000ull) {
            int load = t >= 5 * S && t < 15 * S, fade = t >= 15 * S && t < 16 * S, pause = t >= 11 * S && t < 12500 * MS;
            if (load && !pause) b += 25ull * 1048576 * 22222 / 1000000;
            uint64_t draws = load || fade ? 20 + rnd(21) : 500 + rnd(50);
            if (!load && !fade && t >= 16 * S && !first_full) first_full = t;
            int r = load_det_present(&d, t, b, draws); if (r > 0) { ons++; on_at = t; } if (r < 0) { offs++; off_at = t; }
        }
        CHECK(ons == 1 && offs == 1, "#112 load: one load (began %d, ended %d)", ons, offs);
        CHECK(on_at >= 5 * S && on_at < 5 * S + 800 * MS, "#112 load: seen %.2f s after it began (15 sparse presents and 0.5 s of reads)", (on_at - 5 * S) / 1e9);
        CHECK(off_at == first_full, "#112 load: over at the first full frame (%.2f s, the world at %.2f s)", off_at / 1e9, first_full / 1e9);
    }
    {   // a menu: 30 draws at 60 fps, no reads but a 2 MB save-file read in 0.2 s
        load_det d = { 0 }; uint64_t b = 0; int fired = 0;
        for (uint64_t t = 16 * MS; t < 20 * S; t += 16667 * 1000ull) { if (t >= 10 * S && t < 10200 * MS) b += 2ull * 1048576 * 16667 / 200000; if (load_det_present(&d, t, b, 30) > 0) fired++; }
        CHECK(!fired, "a menu (30 draws, one 2 MB read): no load (fired %d times)", fired);
    }
    {   // a frozen picture: play, then no present for 2 s while the game reads 100 MB/s, then the world again; android_main.c polls every 500 ms
        load_det d = { 0 }; uint64_t b = 0, t = 0; int ons = 0, offs = 0;
        for (t = 16 * MS; t < 3 * S; t += 16667 * 1000ull) load_det_present(&d, t, b, 500);
        uint64_t froze = t;
        for (uint64_t p = 0; p < 2 * S; p += 100 * MS) { b += 10ull * 1048576; if ((p % (500 * MS)) == 0 && load_det_poll(&d, froze + p, b) > 0) ons++; }
        int on_mid = d.on;
        if (load_det_present(&d, froze + 2 * S, b, 500) < 0) offs++;
        CHECK(ons == 1 && on_mid, "a frozen picture with reads going on: a load (by the poll)");
        CHECK(offs == 1 && !d.on, "a frozen picture: over at the next full frame");
    }
    {   // the quiet end: a load whose reads stop while the picture stays sparse (a menu after it): over about 3 s later
        load_det d = { 0 }; uint64_t b = 0, off_at = 0; int ons = 0;
        for (uint64_t t = 16 * MS; t < 15 * S; t += 16667 * 1000ull) {
            if (t < 4 * S) b += 30ull * 1048576 * 16667 / 1000000;
            int r = load_det_present(&d, t, b, 30); if (r > 0) ons++; if (r < 0 && !off_at) off_at = t;
        }
        CHECK(ons == 1 && off_at > 7 * S && off_at < 8 * S, "reads stop, picture stays sparse: over at %.2f s (reads stopped at 4 s)", off_at / 1e9);
    }
    {   // the hold of a load's frames
        uint64_t p30 = 33333333, r60 = 16666667;
        CHECK(load_pace_period(LP_GAME, 1, p30, r60) == p30, "game: the game's period in a load");
        CHECK(load_pace_period(LP_DISPLAY, 1, p30, r60) == r60, "display: one refresh in a load");
        CHECK(load_pace_period(LP_DISPLAY, 0, p30, r60) == p30, "display: the game's period outside a load");
        CHECK(load_pace_period(LP_DISPLAY, 1, r60, 33333333) == r60, "display: never longer than the game's own");
        CHECK(load_pace_period(LP_FREE, 1, p30, r60) == 0, "free: no hold in a load");
        CHECK(load_pace_period(LP_FREE, 1, 0, r60) == 0, "uncapped stays uncapped");
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
