// A load's boost (boost_plan.h, fable-loads.md L3): pin to the fast cores or raise the priority only, on the core tables of phones in the players' logs (each
//   core's top clock as cpuinfo_max_freq gives it, kHz). Expected values are hand-written from the 70 % rule.
// Linux / WSL / macOS: cc -O1 -Wall -o /tmp/t_boost_plan t_boost_plan.c && /tmp/t_boost_plan
#include "../boost_plan.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    static const long sd888[8] = { 2841600, 2419200, 2419200, 2419200, 1804800, 1804800, 1804800, 1804800 },   // 1+3+4, little cores at 64 %
                      tensor[8] = { 1803000, 1803000, 1803000, 1803000, 2253000, 2253000, 2802000, 2802000 },  // 4+2+2 (Pixel 6), little cores at 64 %
                      ex9611[8] = { 2314000, 2314000, 2314000, 2314000, 1742000, 1742000, 1742000, 1742000 },  // 4+4, little cores at 75 %
                      elite[8] = { 4320000, 4320000, 3532800, 3532800, 3532800, 3532800, 3532800, 3532800 },   // 2+6 (Snapdragon 8 Elite), at 82 %
                      g99[8] = { 2200000, 2200000, 2000000, 2000000, 2000000, 2000000, 2000000, 2000000 };     // 2+6 (Helio G99), at 91 %
    int all, f;
    f = boost_fast_cores(sd888, 8, &all);
    CHECK(f == 4 && all == 8, "1+3+4 (Snapdragon 888): 4 fast cores of 8 (%d of %d)", f, all);
    CHECK(boost_pin(BOOST_AUTO, f, all, 2, 2), "1+3+4: 2 busy + 2 pinned fit 4 fast cores: pinned");
    CHECK(!boost_pin(BOOST_AUTO, f, all, 6, 2), "1+3+4: 6 busy + 2 pinned (a load's job threads): priority only, every core kept");
    CHECK(boost_pin(BOOST_PIN, f, all, 6, 2), "1+3+4, DM_LOAD_BOOST=pin: pinned as before");
    CHECK(!boost_pin(BOOST_NICE, f, all, 1, 0), "1+3+4, DM_LOAD_BOOST=nice: never pinned");
    f = boost_fast_cores(tensor, 8, &all);
    CHECK(f == 4 && boost_pin(BOOST_AUTO, f, all, 3, 1) && !boost_pin(BOOST_AUTO, f, all, 4, 1), "4+2+2 (Tensor): 4 fast; 3 + 1 pinned, 4 + 1 not");
    f = boost_fast_cores(ex9611, 8, &all);
    CHECK(f == 8 && !boost_pin(BOOST_AUTO, f, all, 2, 0) && !boost_pin(BOOST_PIN, f, all, 2, 0), "4+4 (Exynos 9611): no slow cores by the rule: no mask (%d fast)", f);
    f = boost_fast_cores(elite, 8, &all);
    CHECK(f == 8 && !boost_pin(BOOST_AUTO, f, all, 9, 2), "2+6 (8 Elite): every core fast: no mask");
    f = boost_fast_cores(g99, 8, &all);
    CHECK(f == 8 && !boost_pin(BOOST_AUTO, f, all, 3, 0), "2+6 (Helio G99): every core fast: no mask");
    static const long none[8] = { 0 };
    f = boost_fast_cores(none, 8, &all);
    CHECK(f == 0 && !boost_pin(BOOST_AUTO, f, all, 1, 0) && !boost_pin(BOOST_PIN, f, all, 1, 0), "no clock table readable: no mask");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
