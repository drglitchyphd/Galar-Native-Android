// Renderer stage 3: stores whose content the NEXT frame overwrites unread, learnt (nvn_vk.c, the held frame, DM_VK_FRAME_HOLD=2). The held frame decides each store from
// what the rest of the frame does (fg_rec.h fgr_fate_exact); a store nothing in the frame touches again ("unknown") is a frame-end store, and for a depth / stencil
// buffer that the next frame clears before anything reads it (most depth buffers) it was written for nothing. Which ones those are can only be learnt: the decision is
// made when the frame is submitted, before the next one exists. So, per image (a plain 2D depth / stencil one: one level, one layer, never shown):
//   - at the flush, an unknown store makes the image pending (and notes whether the store was left out: dc);
//   - in the next frames, the image's first event resolves it: a pass clearing every aspect of it over the whole of it (overwritten unread: one more in a row),
//     a pass loading it (needed: the count starts again) or a read of it (sampled, copied: it is read, never left out from then on);
//   - after XF_LEARN overwrites in a row, its unknown stores are left out (DON'T CARE);
//   - a left-out store that is then loaded or read was needed after all (one frame of undefined depth there): never left out again for that image, said in red.
// Pure: no Vulkan, no locks (the caller's); tests/t_xf_learn.c
#pragma once
#include <stdint.h>
enum { XF_LEARN = 60 };
typedef struct { uint16_t streak; uint8_t pend, dc, off, read; } xf_t;
static inline int xf_skip(const xf_t *s) { return !s->off && !s->read && s->streak >= XF_LEARN; }
// at the flush: the image's store at the end of the held work is unknown; dc: it was left out
static inline void xf_pending(xf_t *s, int dc) { s->pend = 1; s->dc = (uint8_t)(dc != 0); }
// a read of the image: 1 when a left-out store was read (wrong: never again)
static inline int xf_read(xf_t *s) { int bad = s->pend && s->dc; s->read = 1; s->streak = 0; s->pend = s->dc = 0; if (bad) s->off = 1; return bad; }
// a pass attaches it: cleared_all, every aspect cleared over the whole image (what was there is not looked at); else it is loaded. 1: a left-out store was loaded
static inline int xf_attach(xf_t *s, int cleared_all) {
    if (!s->pend) return 0;
    int bad = 0;
    if (cleared_all) { if (s->streak < 60000) s->streak++; }
    else { bad = s->dc; s->streak = 0; if (bad) s->off = 1; }
    s->pend = s->dc = 0; return bad;
}
