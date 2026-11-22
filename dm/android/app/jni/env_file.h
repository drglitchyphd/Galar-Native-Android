// The game's start-up settings files (android_main.c host_start; tests: dm/android/app/jni/tests/t_safe_start.c): KEY=VALUE lines, # comments, each set only when
// not set already, so what was read first wins. host_start reads env.txt (the player's settings), then the safe-start ladder sets its level's (safe_start.h), then
// driver-check.env (the driver check's settings for this driver, DriverCheck.java), then the phone profile its defaults.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// who: how the log names the file's values; taken (if not NULL): the keys it set, separated by commas (DM_DRIVER_CHECK_KEYS: vk_quirks.h vkq_env_src)
static void env_file(const char *path, const char *who, char *taken, size_t ntaken) {
    FILE *f = fopen(path, "r"); char ln[512]; if (!f) return;
    while (fgets(ln, sizeof ln, f)) { char *p = ln; while (*p == ' ' || *p == '\t') p++; if (*p == '#' || !*p) continue; char *eq = strchr(p, '='); if (!eq || eq == p) continue;
        *eq = 0; char *v = eq + 1; char *e = strpbrk(v, "\r\n"); if (e) *e = 0;
        if (!getenv(p)) { setenv(p, v, 0); fprintf(stderr, "boot: %s %s=%s\n", who, p, v);
            if (taken) { size_t l = strlen(taken); if (l + strlen(p) + 2 < ntaken) snprintf(taken + l, ntaken - l, "%s%s", l ? "," : "", p); } } }
    fclose(f);
}
