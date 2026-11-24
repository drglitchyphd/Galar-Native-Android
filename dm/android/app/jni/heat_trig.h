// When the phone counts as hot for the heat guard and Graphics Auto (android_main.c heat_guard / graphics_auto), one check every 5 s. Pure function: unit test
//   dm/android/app/jni/tests/t_heat_trig.c feeds it the readings players' logs show.
// Before 2026-11-24 one reading was enough: thermal status moderate or worse, or the headroom forecast 10 s ahead at 0.9 or more. The forecast swings: a RedMagic
//   that had cooled (Arceus #98) and an Infinix (Arceus #90) were cut to the 720p step at status "none" / 0, and on Legends: Arceus that cut came before every
//   crash in #90. Now (DM_HEAT_SUSTAIN, on unless =0): hot only on three checks in a row (15 s) of either
//   - Android's thermal status moderate (2) or worse (battery saver: light (1) or worse), or
//   - the headroom now (forecast 0, not the 10 s guess) at 0.95 or more (battery saver: 0.85) with the status at least light: throttling has started, by Android's own word;
//   - Adreno's GPU clock limit (kgsl) under 60 % of its top clock (a Galaxy S25+ held at 160 MHz of 1200 with the status not raised, 2026-11-20).
//   A55-class phones that really are hot (severe / critical in every log) still count, 10 s later than before. Calm (the guard's way back) is as before: the
//   status light or lower, headroom under 0.75 (battery saver: none and 0.6), the GPU at 90 % of its clock or more
#pragma once

enum { HEAT_STATUS = 1, HEAT_HEADROOM = 2, HEAT_GPU_CAP = 4, HEAT_SUSTAIN_N = 3 };
typedef struct { int st_n, hr_n, cap_n; } heat_trig;

// st: thermal status (-1 unknown); hr: headroom (forecast 0 with sustain, the 10 s forecast without; negative or NaN unknown); cs: the GPU clock limit's share of
//   its top (negative: cannot be read); saver: battery saver on; sustain: DM_HEAT_SUSTAIN. Returns why it is hot (HEAT_* bits; 0: not hot); *calm: a calm reading
static inline int heat_check(heat_trig *h, int st, float hr, float cs, int saver, int sustain, int *calm) {
    if (!(hr == hr)) hr = -1.0f;   // (NaN: Android's answer when headroom is asked more than once a second)
    int st_hot = saver ? st >= 1 : st >= 2;
    int hr_hot = sustain ? st >= 1 && hr >= (saver ? 0.85f : 0.95f) : hr >= (saver ? 0.75f : 0.9f);
    int cap_hot = cs >= 0 && cs < 0.6f;
    h->st_n = st_hot ? h->st_n + 1 : 0; h->hr_n = hr_hot ? h->hr_n + 1 : 0; h->cap_n = cap_hot ? h->cap_n + 1 : 0;
    int need = sustain ? HEAT_SUSTAIN_N : 1;
    if (calm) *calm = (saver ? st <= 0 && (hr < 0 || hr < 0.6f) : st <= 1 && (hr < 0 || hr < 0.75f)) && !(cs >= 0 && cs < 0.9f);
    return (h->st_n >= need ? HEAT_STATUS : 0) | (h->hr_n >= need ? HEAT_HEADROOM : 0) | (h->cap_n >= need ? HEAT_GPU_CAP : 0);
}
// One reading that would be hot on its own (the count of this kind just started): for the log, so a phone that the old rule would have cut shows it
static inline int heat_first(const heat_trig *h) { return h->st_n == 1 || h->hr_n == 1 || h->cap_n == 1; }
