// Renderer stage 3's learning rules (nvn/xf_learn.h): a depth buffer the next frame clears is learnt and then left out; one the next frame loads, or that is read,
// is not; a wrong guess stops it for good.   cc -O1 -I../nvn -o /tmp/t_xf_learn t_xf_learn.c && /tmp/t_xf_learn
#include <stdio.h>
#include "xf_learn.h"
static int fails, checks;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "\xe2\x9c\x93" : "\xe2\x9c\x97", s); } while (0)
// one frame of a game whose depth buffer is cleared at the start of each frame: the flush (unknown store), then the next frame's clear
static void frame_cleared(xf_t *s) { xf_pending(s, xf_skip(s)); xf_attach(s, 1); }
int main(void) {
    xf_t a = { 0 };
    for (int i = 0; i < XF_LEARN - 1; i++) frame_cleared(&a);
    CHECK(!xf_skip(&a), "not left out before XF_LEARN overwrites in a row");
    frame_cleared(&a);
    CHECK(xf_skip(&a), "left out after XF_LEARN overwrites in a row");
    { xf_t b = a; xf_pending(&b, xf_skip(&b)); CHECK(xf_attach(&b, 0) == 1 && b.off && !xf_skip(&b), "a left-out store loaded by the next frame: wrong, never again"); }
    { xf_t b = a; xf_pending(&b, xf_skip(&b)); CHECK(xf_read(&b) == 1 && b.off && !xf_skip(&b), "a left-out store read (sampled): wrong, never again"); }
    { xf_t b = { 0 }; for (int i = 0; i < XF_LEARN / 2; i++) frame_cleared(&b);
      xf_pending(&b, 0); CHECK(xf_attach(&b, 0) == 0 && b.streak == 0 && !b.off, "loaded while still learning: no harm, the count starts again"); }
    { xf_t b = { 0 }; xf_read(&b); for (int i = 0; i < 2 * XF_LEARN; i++) frame_cleared(&b);
      CHECK(!xf_skip(&b), "an image that was ever read is never left out"); }
    { xf_t b = a; CHECK(xf_attach(&b, 1) == 0 && b.streak == a.streak, "an attach with nothing pending (inside one frame) changes nothing"); }
    { xf_t b = a; xf_pending(&b, 1); xf_pending(&b, 1); CHECK(xf_attach(&b, 1) == 0 && xf_skip(&b), "untouched for a frame (still pending), then cleared: still learnt"); }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
