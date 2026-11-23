// nn::time: a real device gets the network-calibrated user clock through the time service; the host reads the system clock directly (single machine, offline).
// Layout follows the public Horizon ABI: PosixTime = int64 seconds; CalendarTime = {s16 year, s8 month/day/hour/minute/second};
// CalendarAdditionalInfo = {u32 day of week, u32 day of year, char time zone name[8], u32 DST, s32 UTC offset in seconds}.
#include "hle.h"
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <strings.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

typedef struct { int16_t year; int8_t month, day, hour, minute, second; int8_t pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char tzname[8]; uint32_t dst; int32_t offset; } CalendarAdditionalInfo;

// DM_FIXED_TIME=seconds (for deterministic comparison): the user clock always returns this value (and main.c pins the host time zone to UTC)
static int64_t fixed_time(void) {
    static int64_t v = -2;
    if (v == -2) { const char *e = getenv("DM_FIXED_TIME"); v = e ? strtoll(e, 0, 0) : -1;
                   if (v >= 0) LOG("DM_FIXED_TIME=%lld: the user clock returns a fixed value", (long long)v); }
    return v;
}
HLE_FORCE("_ZN2nn4time10InitializeEv", "connect to the time service; clocks come from the host system time instead", Result, h_time_Initialize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4time13IsInitializedEv", "the time service is taken over, always initialized", int, h_time_IsInitialized, void) { return 1; }
// Steady clock (seconds + a clock source ID, a 16-byte UUID). A Switch's runs on through sleep and power-off and only goes forward, so games keep its time points in their
// saves and measure real time with them (Sword / Shield: GetSpanBetween at main+0x5c58f8; a failed or short span = "the clock is not to be trusted"). It used to be the
// host's CLOCK_MONOTONIC: stopped while the phone slept (the user clock then seemed set by hand: CalculateStandardUserSystemClockDifferenceByUser != 0) and started again
// from 0 after a reboot (spans went negative). Now: seconds since boot counting sleep (CLOCK_BOOTTIME) + a base kept next to the save (<DM_SAVE>.clock): the same boot keeps
// the base; after a reboot the clock goes on from the wall clock with the last known steady - wall difference (never backwards). A clock set by hand while the game runs
// moves the user clock only, as on a Switch. GetSpanBetween runs as is in sdk: it subtracts only when the source IDs match. DM_FIXED_TIME: the plain monotonic seconds
#define STEADY_ID "dmhost-steady-02"       // (16 bytes, no terminator used: the clock counting sleep and reboots)
#define STEADY_ID_OLD "dmhost-steady-01"   // (the phone's uptime clock before 2026-11-21, still in older saves)
static int64_t boot_secs(void) {
#ifdef CLOCK_BOOTTIME
    struct timespec t; clock_gettime(CLOCK_BOOTTIME, &t); return (int64_t)t.tv_sec;
#else
    return (int64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000ull);   // (macOS: CLOCK_MONOTONIC counts sleep)
#endif
}
static pthread_mutex_t g_st_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_once_t g_st_once = PTHREAD_ONCE_INIT;
static int64_t g_st_base, g_st_k; static char g_st_boot[64], g_st_path[1100];
static void st_write(int64_t s, int64_t wall) {   // (g_st_mu held, or from st_init)
    g_st_k = s - wall; if (!*g_st_path) return;
    char tmp[1120]; snprintf(tmp, sizeof tmp, "%s.tmp", g_st_path); FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "steady-v1 %s %lld %lld %lld\n", *g_st_boot ? g_st_boot : "-", (long long)g_st_base, (long long)(s - wall), (long long)s); fclose(f); rename(tmp, g_st_path);
}
static void st_init(void) {
    int64_t bt = boot_secs(), wall = time(NULL); const char *how = "new";
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r"); if (f) { if (fgets(g_st_boot, sizeof g_st_boot, f)) g_st_boot[strcspn(g_st_boot, " \n")] = 0; fclose(f); }
    const char *d = getenv("DM_SAVE");
    if (d && *d) { snprintf(g_st_path, sizeof g_st_path, "%s", d); size_t L = strlen(g_st_path); while (L > 1 && g_st_path[L - 1] == '/') g_st_path[--L] = 0; strlcat(g_st_path, ".clock", sizeof g_st_path); }
    char id[64]; long long base = 0, k = 0, last = 0; int ok = 0;
    if (*g_st_path && (f = fopen(g_st_path, "r"))) { ok = fscanf(f, "steady-v1 %63s %lld %lld %lld", id, &base, &k, &last) == 4; fclose(f); }
    if (ok && *g_st_boot && !strcmp(id, g_st_boot)) { g_st_base = base; how = "same phone boot"; }
    else if (ok) { int64_t s = wall + k; if (s < last) s = last; g_st_base = s - bt; how = "the phone restarted since"; }
    else g_st_base = wall - bt;
    st_write(g_st_base + bt, wall);
    LOG("time: steady clock %lld s (%s; counts sleep, kept in %s)", (long long)(g_st_base + bt), how, *g_st_path ? g_st_path : "memory only");
}
static int64_t steady_secs(void) {
    if (fixed_time() >= 0) return (int64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000ull);
    pthread_once(&g_st_once, st_init);
    int64_t s = g_st_base + boot_secs(), wall = time(NULL);
    if (llabs((s - wall) - g_st_k) > 2) {   // the wall clock was set (by hand or by the network): keep the new difference for the next reboot
        pthread_mutex_lock(&g_st_mu); if (llabs((s - wall) - g_st_k) > 2) st_write(s, wall); pthread_mutex_unlock(&g_st_mu); }
    return s;
}
HLE_FORCE("_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE", "steady clock → host seconds counting sleep and reboots, fixed source ID", Result, h_time_SteadyNow, uint8_t *out) {
    int64_t s = steady_secs();
    memcpy(out, &s, 8); memcpy(out + 8, STEADY_ID, 16); return RES_OK;
}
// Time between two steady time points (Sword / Shield main+0x5c58f8, before the clock check below). The SDK's subtracts when the source IDs match and fails otherwise;
// a time point from the old host clock (STEADY_ID_OLD: phone uptime, kept in saves from before) against the new one counts as time gone forward (1 s), not a failure,
// so the update itself does not look like a clock change
HLE_FORCE("_ZN2nn4time14GetSpanBetweenEPlRKNS0_20SteadyClockTimePointES4_", "steady clock span; the old host clock's time points count as earlier", Result, h_time_Span, int64_t *out, const uint8_t *a, const uint8_t *b) {
    int64_t va, vb; memcpy(&va, a, 8); memcpy(&vb, b, 8);
    if (!memcmp(a + 8, b + 8, 16)) { *out = vb - va; return RES_OK; }
    if (!memcmp(a + 8, STEADY_ID_OLD, 16) && !memcmp(b + 8, STEADY_ID, 16)) { *out = 1; return RES_OK; }
    if (!memcmp(a + 8, STEADY_ID, 16) && !memcmp(b + 8, STEADY_ID_OLD, 16)) { *out = -1; return RES_OK; }
    return RES_FAIL;
}
// The SDK's now() asks the time service object, which the host never makes (Initialize is taken over): it returned 0 every time. Sword / Shield call it from 19 places
// (main+0x10f65c4, +0x11182b4 ...): their real-time timers stood still
HLE_FORCE("_ZN2nn4time19StandardSteadyClock3nowEv", "steady clock seconds (the time service is not opened: the SDK's returned 0)", int64_t, h_time_SteadyNowS, void) { return steady_secs(); }
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "user clock → host time()", Result, h_time_GetCurrentTime, int64_t *out) {
    int64_t ft = fixed_time(); *out = ft >= 0 ? ft : time(NULL); return RES_OK;
}
// The C library's real-time clock (SDK 17): clock_gettime(CLOCK_REALTIME) calls the source that nntimeStartup selects once time::Initialize succeeds (sdk+0x8e414: 1 = the standard user clock,
// 2 = the adjustable one; 0, the system tick, before that), and std::chrono::system_clock::now() throws on its failure. The SDK's getters ask the time service: Ace Attorney Investigations
// Collection called std::terminate at its first real-time read, right after the first-boot language dialog (main+0x3f3f68; found by armyja, GitHub issue #18). The host user clock, to the nanosecond
static int h_time_timespec(int64_t *ts) {
    int64_t ft = fixed_time();
    if (ft >= 0) { ts[0] = ft; ts[1] = 0; return 0; }
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t); ts[0] = t.tv_sec; ts[1] = t.tv_nsec; return 0;
}
HLE_FORCE("_ZN2nn4time41GetStandardUserSystemClockCurrentTimespecEP8timespec", "C library real-time clock from the user clock → host real time", int, h_time_StdTimespec, int64_t *ts) { return h_time_timespec(ts); }
HLE_FORCE("_ZN2nn4time43GetAdjustableUserSystemClockCurrentTimespecEP8timespec", "C library real-time clock from the adjustable user clock → host real time", int, h_time_AdjTimespec, int64_t *ts) { return h_time_timespec(ts); }
// User clock context (0x20 bytes: s64 offset + the steady time point it was made at; user time = offset + a steady time point of the same source ID, the snapshot below fills its +0x00 the same way).
// The SDK reads it from the time service's shared memory, which the host never opens: it returns 0x74 and writes nothing. Unity's DateTime.UtcNow (Engage main+0x4ad40c; Ace Attorney Investigations
// 1&2 per PR #14) then never starts its clock and reads 0001-01-01. Unity takes its time from GetCurrentTime plus system ticks and only compares the context to notice the clock being set, so like a
// Switch's it stays the same until the host clock moves against the guest ticks by more than a second (clock set by hand, a sleep mach_absolute_time did not count); with DM_FIXED_TIME it is made once
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", "user clock context from the time service; the host's user clock over its steady clock, renewed when the host clock is set",
          Result, h_time_GetContext, uint8_t *out) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static uint8_t ctx[0x20]; static int64_t drift; static int made;
    uint8_t tp[0x18]; int64_t now; h_time_SteadyNow(tp); h_time_GetCurrentTime(&now);
    int64_t d = now - (int64_t)(guest_ns() / 1000000000ull);
    pthread_mutex_lock(&mu);
    if (!made || (fixed_time() < 0 && llabs(d - drift) > 1)) {
        int64_t off = now - *(const int64_t *)tp; memcpy(ctx, &off, 8); memcpy(ctx + 8, tp, 0x18);
        if (made) LOG("time: host clock moved %+lld s against the guest ticks, new user clock context", (long long)(d - drift));
        drift = d; made = 1;
    }
    memcpy(out, ctx, 0x20);
    pthread_mutex_unlock(&mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock3nowEv", "user clock → host time()", int64_t, h_time_now, void) {
    int64_t ft = fixed_time(); return ft >= 0 ? ft : time(NULL); }
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock9to_time_tERKNSt3__16chrono10time_pointINS0_17SystemClockTraitsENS3_8durationIlNS2_5ratioILl1ELl1EEEEEEE",
          "the SDK implementation pairs with now(), taken over with it", int64_t, h_time_to_time_t, const int64_t *tp) { return *tp; }
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
          "time zone rules of the time service; uses the host localtime_r instead", Result, h_time_ToCalendarTime, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt) {
    time_t t = *pt; struct tm tm; localtime_r(&t, &tm);
    static int once; if (!once++) LOG("ToCalendarTime: first conversion %lld → %04d-%02d-%02d %02d:%02d (time zone %s, offset %ld s)", (long long)t, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_zone ? tm.tm_zone : "?", (long)tm.tm_gmtoff);
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) { memset(ai, 0, sizeof *ai); ai->wday = tm.tm_wday; ai->yday = tm.tm_yday; strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname);
              ai->dst = tm.tm_isdst > 0; ai->offset = (int32_t)tm.tm_gmtoff; }
    return RES_OK;
}
// Explicit time zone rules (Monster Hunter Rise, SDK 11): LoadTimeZoneRule reads the rule from the time service; the host keeps the game's rule buffer untouched and converts with localtime_r as above
// The device's time zone name (LocationName, char[0x24]): the phone's zone ID (Android persist.sys.timezone, "Europe/London"), else TZ, else the zone's abbreviation. Not the
// abbreviation first: in the UK it reads "GMT", the very name games load to mean UTC (below)
void dm_time_location(char *out) {
    memset(out, 0, 0x24);
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = ""; if (__system_property_get("persist.sys.timezone", v) > 0 && *v) { strlcpy(out, v, 0x24); return; }
#endif
    const char *tz = getenv("TZ"); if (tz && *tz && *tz != ':') { strlcpy(out, tz, 0x24); return; }
    tzset(); strlcpy(out, tzname[0] && *tzname[0] ? tzname[0] : "UTC", 0x24);
}
// Explicit time zone rules: LoadTimeZoneRule reads the named zone's rule from the time service. All three Pokémon games load "GMT" (Sword / Shield main+0x5c5848, Legends:
// Arceus, Scarlet / Violet) and turn the local calendar back into seconds with it (ToPosixTime): "local time as seconds", their day counter. Converting with the device zone
// instead gave plain UTC seconds, so the game's day began at midnight UTC: hours off for every player not on UTC (daily events, the day's resets, time of day). The host
// marks the rule buffer (0x4000 bytes, the game's) with the zone name: a UTC name that is not the device's own converts as UTC, any other as the device zone
#define TZ_MARK "dm-tzrule-v1"
static int rule_utc(const void *rule) {
    const char *r = rule; if (!r || memcmp(r, TZ_MARK, sizeof TZ_MARK)) return 0;
    const char *n = r + sizeof TZ_MARK; char dev[0x24]; dm_time_location(dev); if (!strncmp(n, dev, 0x24)) return 0;
    static const char *const utc[] = { "GMT", "UTC", "UCT", "GMT0", "GMT+0", "GMT-0", "Etc/GMT", "Etc/UTC", "Etc/UCT", "Etc/GMT0", "Etc/GMT+0", "Etc/GMT-0", "Etc/Universal",
                                       "Etc/Zulu", "Etc/Greenwich", "Universal", "Zulu", "Greenwich" };
    for (size_t i = 0; i < sizeof utc / sizeof *utc; i++) if (!strncasecmp(n, utc[i], 0x24)) return 1;
    return 0;
}
HLE_FORCE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", "time zone rule from the time service; the host marks the buffer with the zone name (UTC names convert as UTC, others as the device zone)",
          Result, h_time_LoadRule, void *rule, const char *name) {
    char n[0x24] = ""; if (name) strncpy(n, name, sizeof n - 1);
    if (rule) { memcpy(rule, TZ_MARK, sizeof TZ_MARK); memcpy((char *)rule + sizeof TZ_MARK, n, sizeof n); }
    static int once; if (!once++) LOG("time::LoadTimeZoneRule %.36s: %s (said once)", n, rule_utc(rule) ? "converts as UTC" : "converts with the device time zone");
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE", "conversion with an explicit time zone rule (UTC rule: gmtime_r; else the device zone)",
          Result, h_time_ToCalendarTimeRule, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt, const void *rule) {
    if (!rule_utc(rule)) return h_time_ToCalendarTime(ct, ai, pt);
    time_t t = *pt; struct tm tm; gmtime_r(&t, &tm);
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) { memset(ai, 0, sizeof *ai); ai->wday = tm.tm_wday; ai->yday = tm.tm_yday; strlcpy(ai->tzname, "UTC", sizeof ai->tzname); }
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", "network clock accuracy (time service): offline, never sufficient", unsigned, h_time_NetAccurate, void) { return 0; }
// Clock snapshot (Tears of the Kingdom: CreateWithStandardSystemClock asks the time service, which the host never opens → abort in sm::Initialize). 0xD0 bytes, laid out
// as Ryujinx lays out the service's ClockSnapshot and as this SDK's constructor clears it (its last store is the byte at +0xcd): +0x00 user clock context {offset,
// steady time point}, +0x20 network clock context, +0x40 user time, +0x48 network time, +0x50 / +0x58 user / network calendar, +0x60 / +0x78 their additional info,
// +0x90 steady time point, +0xa8 location name (0x24), +0xcc automatic correction, +0xcd type. Offline, the network fields stay 0, so the SDK's CalculateSpanBetween
// uses the steady time points (same source ID)
HLE_FORCE("_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", "snapshot from the time service; the host fills it on the spot (offline: network clock 0)", Result, h_time_Snapshot, uint8_t *s) {
    memset(s, 0, 0xd0);
    uint8_t tp[0x18]; h_time_SteadyNow(tp);
    int64_t now, off; h_time_GetCurrentTime(&now); off = now - *(const int64_t *)tp;
    memcpy(s, &off, 8); memcpy(s + 0x08, tp, 0x18); memcpy(s + 0x40, &now, 8);
    h_time_ToCalendarTime((CalendarTime *)(s + 0x50), (CalendarAdditionalInfo *)(s + 0x60), &now);
    memcpy(s + 0x90, tp, 0x18);
    tzset(); strlcpy((char *)s + 0xa8, tzname[0] && *tzname[0] ? tzname[0] : "UTC", 0x24);   // (the zone's abbreviation, as before: saves keep snapshots, and Sword /
    return RES_OK;                                                                             //  Shield count another location name as a clock change, main+0x5c5970)
}
// How far the user clock was moved between two snapshots (Pokémon Shield 1.3.2, SDK 7.3.2, main+0x5c5920, a few minutes into the intro): the sdk's version aborts because the time service object is null
// (the host takes over Initialize) and would ask the service. As the service computes it (Ryujinx): the change of the user clock's offset over its steady clock, in seconds; 0 when the
// snapshots come from different steady clock sources or both have automatic correction on. On the host the offset moves only when the system clock is changed. Returns a TimeSpan (ns)
HLE_FORCE("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", "user clock change between two snapshots (time service); computed from the snapshots",
          int64_t, h_time_UserDiff, const uint8_t *a, const uint8_t *b) {
    int64_t oa, ob; memcpy(&oa, a, 8); memcpy(&ob, b, 8);
    if (memcmp(a + 0x10, b + 0x10, 0x10) || (a[0xcc] && b[0xcc])) return 0;   // steady clock source IDs (+0x08 time point: value, then the 16-byte source ID); automatic correction
    // Under 2 minutes is the phone's own network time correction (and a second of rounding between the two clocks), not the player: Sword / Shield take ANY difference as a
    // clock change (main+0x5c5920) and lock their timed events for 24 hours (main+0x1419a30: 1440 minutes, counted down by main+0x1419b00) - Poké Jobs and berry trees
    // stood still. A date or hour set by hand still counts, as on a Switch
    if (llabs(ob - oa) < 120) return 0;
    return (ob - oa) * 1000000000;
}
// The reverse: local calendar → absolute seconds (13 Sentinels). Around a daylight-saving switch one local time maps to 0 or 2 absolute seconds; the host's mktime gives one
HLE_FORCE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE",
          "time zone rules of the time service; uses the host mktime instead", Result, h_time_ToPosixTime, int *count, int64_t *out, int n, const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second, .tm_isdst = -1};
    time_t t = mktime(&tm);
    if (count) *count = n > 0;
    if (n > 0 && out) out[0] = (int64_t)t;
    return RES_OK;
}
// With an explicit time zone rule (Pokémon Legends: Z-A, SDK 19; the rule came from the host's LoadTimeZoneRule above and stays unused): the device zone, as for ToCalendarTime
HLE_FORCE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", "conversion with an explicit time zone rule (UTC rule: timegm; else the device zone, mktime)",
          Result, h_time_ToPosixTimeRule, int *count, int64_t *out, int n, const CalendarTime *ct, const void *rule) {
    if (!rule_utc(rule)) return h_time_ToPosixTime(count, out, n, ct);
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second};
    if (count) *count = n > 0;
    if (n > 0 && out) out[0] = (int64_t)timegm(&tm);
    return RES_OK;
}
// UTC calendar → absolute seconds (Super Mario Odyssey); no time zone involved, the host's timegm
HLE_FORCE("_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", "UTC calendar → absolute seconds; host timegm", int64_t, h_time_ToPosixUtc, const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second};
    return (int64_t)timegm(&tm);
}
// Local time conversion is forced onto the host libc: the SDK's own localtime / localtime_r / mktime have no time zone data and always use UTC
// (a game's local timestamps were off by the time zone's offset). The SDK on a real Switch converts by the system time zone, so only following the host system time zone here is consistent;
// the host TZ normally = the system time zone, and is pinned to UTC only for deterministic comparison with DM_FIXED_TIME (main.c). The struct tm layout is the same on both sides (9 ints, long offset, time zone name pointer).
// gmtime / gmtime_r / timegm are UTC anyway and are not taken over. The same-named pass-through entries in hle_table.c only take effect when the symbol has no definition (the SDK defines them), hence HLE_FORCE
HLE_FORCE("localtime", "local time → host localtime (follows the system time zone)", struct tm *, h_localtime, const int64_t *t) { time_t v = (time_t)*t; return localtime(&v); }
HLE_FORCE("localtime_r", "local time → host localtime_r (follows the system time zone)", struct tm *, h_localtime_r, const int64_t *t, struct tm *out) { time_t v = (time_t)*t; return localtime_r(&v, out); }
HLE_FORCE("mktime", "local calendar → absolute seconds, host mktime (follows the system time zone)", int64_t, h_mktime, struct tm *tm) { return (int64_t)mktime(tm); }
