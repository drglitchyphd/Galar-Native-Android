// Scarlet / Violet 4.0.0 up to 60 fps (DM_FPS_TARGET=60 or Dynamic): our own patch, from the game's frame code. Included by loader.c and by the PC test
//   dm/tests/test_paldea_fps.c, so both check the same words. The two mains are one program (the same bytes at every site here, checked in both).
//
// How the game times a frame (main+0x2b7a7a0 .. +0x2b7a9c0, run once a frame):
//   - a byte of its frame settings (LDRSB W20, [X10, #0x20]) is the frame-time index: 1 on the Switch (30 fps). The present interval it asks NVN for is that
//     index + 1 (ADD W1, W20, #1 at +0x2b7a874, then NVN's set-present-interval through the pointer at +0x46e0bd8): 2 vsyncs, 30 fps;
//   - the frame step: the u64 nanoseconds of a table by index, main+0x3c2fba8 = { 16666667, 33333334, 50000000, 66666667, 83333334 } (read through the GOT slot
//     at +0x46d8bc0), entry [index] copied into each of the game's step slots every frame (the setter at +0x92dd20 / +0x2b7b308, called at +0x2b7a8f4 and
//     +0x2b7a924). Entry [1], +0x3c2fbb0, is the step of every frame on the Switch;
//   - one consumer divides a step slot by (entry [index] - 1) (+0xd96428 .. +0xd96454: LDR X8, [X8, X20, LSL #3]; SUB X8, X8, #1; then FDIV, FCVT S8) and hands
//     the quotient on as a float (a virtual call, +0x58): this frame's time in frames of 30 fps (1.0 on the Switch, more when a frame ran late).
// Ours:
//   1. present interval 1: +0x2b7a874 ADD W1, W20, #1 → MOV W1, #1. The host then paces presents in whole vsyncs, 1 (60 fps) or 2 (30 fps) (hle_nvn.c dyn_tick);
//   2. the step is the time the frame really takes: the host writes entry [1] (+0x3c2fbb0, data, not code) every frame from the hold it presents with:
//      16666667 ns at 1 vsync, 33333334 (the game's own value) at 2, never more: never faster than real time, and under 30 fps it slows as on the Switch;
//   3. the quotient keeps its unit, frames of 30 fps: the divisor becomes the game's own one at 30, 33333333 (entry [1] - 1), not the rewritten entry:
//      +0xd9643c / +0xd96440 LDR X8, [X8, X20, LSL #3]; SUB X8, X8, #1 → MOVZ X8, #0xa055; MOVK X8, #0x1fc, LSL #16. At 2 vsyncs every value is the game's own.
// At 30 (the default) nothing here is written: the main stays byte for byte the game's. Every word is checked first, in whole or not at all.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

static const char *const PFPS_BUILDS[] = { "421c5411b487eb4d049dd065fec95477", "709bfd66115298640155fcc4979dba15" };   // Scarlet 4.0.0, Violet 4.0.0
typedef struct { uint32_t off, orig, ours; const char *what; } pfps_site;
static const pfps_site PFPS_CODE[] = {
    { 0x2b7a874, 0x11000681u, 0x52800021u, "present interval: ADD W1, W20, #1 → MOV W1, #1" },
    { 0xd9643c, 0xf8747908u, 0xd2940aa8u, "frame-time divisor: LDR X8, [X8, X20, LSL #3] → MOVZ X8, #0xa055" },
    { 0xd96440, 0xd1000508u, 0xf2a03f88u, "frame-time divisor: SUB X8, X8, #1 → MOVK X8, #0x1fc, LSL #16 (33333333)" },
};
#define PFPS_NCODE ((int)(sizeof PFPS_CODE / sizeof *PFPS_CODE))
#define PFPS_STEP 0x3c2fbb0ull   // the frame-time table's entry [1], u64 ns
#define PFPS_STEP_30 33333334ull   // its value in the game (the step at 30 fps, and the most the host ever writes)
#define PFPS_STEP_60 16666667ull

static int pfps_build(const char *build_id) {
    for (unsigned i = 0; i < sizeof PFPS_BUILDS / sizeof *PFPS_BUILDS; i++) if (build_id && !strncasecmp(build_id, PFPS_BUILDS[i], 32)) return 1;
    return 0;
}
// asked for: Frame rate 60 or Dynamic (android_main.c turns "dyn" into 60), as the setting always was; 30, Auto (30 for Scarlet / Violet) or nothing: no
static int pfps_wanted(const char *fps_target) { return fps_target && atoi(fps_target) >= 60; }
// -1 when every word holds the game's own value (code sites inside the code area, the step in the data after it), else the failing site (PFPS_NCODE: the step)
static int pfps_check(const uint8_t *base, uint64_t lim, uint64_t text_end) {
    for (int i = 0; i < PFPS_NCODE; i++) { uint32_t w;
        if (PFPS_CODE[i].off + 4 > text_end || PFPS_CODE[i].off + 4 > lim) return i;
        memcpy(&w, base + PFPS_CODE[i].off, 4); if (w != PFPS_CODE[i].orig) return i; }
    uint64_t s; if (PFPS_STEP < ((text_end + 0x3fff) & ~0x3fffull) || PFPS_STEP + 8 > lim) return PFPS_NCODE;   // (past the sealed code pages: written every frame)
    memcpy(&s, base + PFPS_STEP, 8); return s == PFPS_STEP_30 ? -1 : PFPS_NCODE;
}
static void pfps_write_code(uint8_t *base) { for (int i = 0; i < PFPS_NCODE; i++) memcpy(base + PFPS_CODE[i].off, &PFPS_CODE[i].ours, 4); }
// the step for a frame held k vsyncs of 60 Hz (0: not decided yet): the game's own 30 fps value at most
static uint64_t pfps_step_ns(int k) { return k == 1 ? PFPS_STEP_60 : PFPS_STEP_30; }
