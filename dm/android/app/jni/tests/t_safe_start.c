// Safe start unit test (safe_start.h): which starts move a level up (a crash in the graphics driver before 30 s of play, the device up) and which do not (the game's own
// code, before the device, after play, with mods on), the level kept across good starts, started over for another driver or app version, the settings each level sets
// under the player's own, and the top level; then the order host_start reads its settings in (env_file.h: env.txt, the ladder, driver-check.env) and the keys the
// driver check set. A scratch folder stands in for the app's files. Expected values are hand-written.
// Linux / WSL / macOS: cc -o /tmp/t_safe_start t_safe_start.c && /tmp/t_safe_start
#include "../safe_start.h"
#include "../env_file.h"
#include <sys/stat.h>

static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", s); } while (0)
static char D[256];
static void put(const char *name, const char *text) {
    char p[400]; snprintf(p, sizeof p, "%s/%s", D, name);
    if (!text) { unlink(p); return; }
    FILE *f = fopen(p, "w"); fputs(text, f); fclose(f);
}
static const char *get(const char *name) {
    static char b[512]; char p[400]; snprintf(p, sizeof p, "%s/%s", D, name); b[0] = 0;
    FILE *f = fopen(p, "r"); if (!f) return NULL; size_t k = fread(b, 1, sizeof b - 1, f); b[k] = 0; fclose(f); return b;
}
static void fresh_env(const char *ver, const char *drv) {
    unsetenv("DM_VK_TEX_QUALITY"); unsetenv("DM_VK_UBO_SNAPSHOT"); unsetenv("DM_VK_BC_GPU"); unsetenv("DM_VK_BC_TRANSCODE"); unsetenv("DM_VK_FLOAT_CONTROLS");
    if (ver) setenv("DM_APP_VERSION", ver, 1); else unsetenv("DM_APP_VERSION");
    if (drv) setenv("DM_VK_DRIVER", drv, 1); else unsetenv("DM_VK_DRIVER");
}
static int is(const char *k, const char *v) { const char *e = getenv(k); return v ? e && !strcmp(e, v) : !e; }
static const char *CRASH = "start driver stock mods 0\ndevice\ncrashed\n";

int main(void) {
    snprintf(D, sizeof D, "/tmp/t_safe_start.%d", (int)getpid()); mkdir(D, 0700);
    put("gpu.txt", "Mali-G615 MC2\n");

    fresh_env("55", NULL); put("boot_state", NULL);
    CHECK(safe_start(D) == 0 && !get("safe_start") && is("DM_VK_TEX_QUALITY", NULL), "no crash, no level: nothing set");

    fresh_env("55", NULL); put("boot_state", CRASH);
    CHECK(safe_start(D) == 1, "a crash in the driver after the device came up, before play: a level up");
    CHECK(get("safe_start") && !strcmp(get("safe_start"), "1\nstock|55\n"), "level 1 kept for the phone's own driver and version 55");
    CHECK(is("DM_VK_TEX_QUALITY", "full") && is("DM_VK_UBO_SNAPSHOT", NULL) && is("DM_VK_BC_GPU", NULL), "level 1: full textures only");

    fresh_env("55", NULL); put("boot_state", CRASH);
    CHECK(safe_start(D) == 1 && !strcmp(get("safe_start"), "2\nstock|55\n"), "crashed again: level 2");
    CHECK(is("DM_VK_TEX_QUALITY", "full") && is("DM_VK_UBO_SNAPSHOT", "1") && is("DM_VK_BC_GPU", NULL), "level 2: also uniform buffers copied (#105)");

    fresh_env("55", NULL); put("boot_state", NULL);   // (a start that played removes boot_state)
    CHECK(safe_start(D) == 0 && !strcmp(get("safe_start"), "2\nstock|55\n"), "a good start: the level is kept");
    CHECK(is("DM_VK_TEX_QUALITY", "full") && is("DM_VK_UBO_SNAPSHOT", "1"), "a good start: level 2's settings still set");

    static const struct { const char *state, *what; } no[] = {
        { "start driver stock mods 0\ndevice\ncrashed in the game's code\n", "a crash in the game's own code: not the driver's, no level up" },
        { "start driver stock mods 0\ncrashed\n", "a crash before the graphics device came up: no level up" },
        { "start driver stock mods 0\ndevice\nplaying\ncrashed\n", "a crash after 30 s of play: no level up" },
        { "start driver stock mods 2\ndevice\ncrashed\n", "a crash with mods on (they are switched off instead): no level up" },
    };
    for (unsigned i = 0; i < sizeof no / sizeof *no; i++) {
        fresh_env("55", NULL); put("boot_state", no[i].state);
        CHECK(safe_start(D) == 0 && !strcmp(get("safe_start"), "2\nstock|55\n"), no[i].what);
    }

    fresh_env("56", NULL); put("boot_state", NULL);
    CHECK(safe_start(D) == 0 && !get("safe_start") && is("DM_VK_TEX_QUALITY", NULL) && is("DM_VK_BC_GPU", NULL), "an update (version 56): the normal settings again");
    fresh_env("56", NULL); put("boot_state", CRASH);
    CHECK(safe_start(D) == 1 && !strcmp(get("safe_start"), "1\nstock|56\n"), "then a crash on version 56: level 1 for it");

    fresh_env("56", "/data/drivers/panvk/libvulkan_panfrost.so"); put("boot_state", NULL);
    CHECK(safe_start(D) == 0 && !get("safe_start") && is("DM_VK_TEX_QUALITY", NULL), "a driver package: the phone's own driver's level does not apply");

    fresh_env("56", NULL); put("safe_start", "1\nstock|56\n"); put("boot_state", CRASH); setenv("DM_VK_TEX_QUALITY", "half", 1);   // (env.txt set it)
    CHECK(safe_start(D) == 1 && is("DM_VK_TEX_QUALITY", "half") && is("DM_VK_UBO_SNAPSHOT", "1"), "the player's own setting (half textures in env.txt) wins; level 2's other setting applies");

    fresh_env("56", NULL); put("safe_start", "5\nstock|56\n"); put("boot_state", CRASH);
    CHECK(safe_start(D) == 0 && !strcmp(get("safe_start"), "5\nstock|56\n"), "at the top level a crash goes no further (the driver dialog shows instead)");
    CHECK(is("DM_VK_TEX_QUALITY", "full") && is("DM_VK_UBO_SNAPSHOT", "1") && is("DM_VK_BC_GPU", "0") && is("DM_VK_BC_TRANSCODE", "off") && is("DM_VK_FLOAT_CONTROLS", "0"), "level 5: all five settings");

    fresh_env("56", NULL); put("safe_start", "9\nstock|56\n"); put("boot_state", NULL);
    CHECK(safe_start(D) == 0 && is("DM_VK_TEX_QUALITY", NULL), "a level out of range in the file: taken as none");

    // the order host_start reads its settings in (env_file.h): env.txt, the ladder, driver-check.env, each set only when not set, and the keys the driver check set
    //   named for nvn_vk.c (DM_DRIVER_CHECK_KEYS: its values are this driver's defaults, the crash markers apply to them)
    {   char p[400], keys[512], m[700]; unsetenv("DM_VK_DMAHEAP"); unsetenv("DM_VK_ZCOPY");
        put("driver-check.env", "# The driver check's settings\nDM_VK_FLOAT_CONTROLS=1\nDM_VK_DMAHEAP=0\nDM_VK_ZCOPY=0\n# tests that took the driver down: depth-copy\n");
        snprintf(p, sizeof p, "%s/driver-check.env", D);
        fresh_env("56", NULL); put("safe_start", "5\nstock|56\n"); put("boot_state", NULL); setenv("DM_VK_ZCOPY", "1", 1);   // (env.txt: the player's own)
        safe_start(D); keys[0] = 0; env_file(p, "driver check", keys, sizeof keys);
        CHECK(is("DM_VK_FLOAT_CONTROLS", "0") && is("DM_VK_DMAHEAP", "0") && is("DM_VK_ZCOPY", "1"), "driver-check.env after the ladder: level 5's no float controls (#89) wins over the check's, env.txt over both");
        snprintf(m, sizeof m, "the keys the driver check set: %s (not the ladder's, not the player's)", keys); CHECK(!strcmp(keys, "DM_VK_DMAHEAP"), m);
        fresh_env("56", NULL); put("safe_start", NULL); unsetenv("DM_VK_DMAHEAP"); unsetenv("DM_VK_ZCOPY");
        safe_start(D); keys[0] = 0; env_file(p, "driver check", keys, sizeof keys);
        snprintf(m, sizeof m, "no ladder level: the check's three settings, named (%s)", keys);
        CHECK(is("DM_VK_FLOAT_CONTROLS", "1") && !strcmp(keys, "DM_VK_FLOAT_CONTROLS,DM_VK_DMAHEAP,DM_VK_ZCOPY"), m);
        char small[24] = ""; unsetenv("DM_VK_FLOAT_CONTROLS"); unsetenv("DM_VK_DMAHEAP"); unsetenv("DM_VK_ZCOPY"); env_file(p, "driver check", small, sizeof small);
        snprintf(m, sizeof m, "a key list that does not fit stops at whole keys (%s)", small); CHECK(!strcmp(small, "DM_VK_FLOAT_CONTROLS"), m);
        unsetenv("DM_VK_DMAHEAP"); unsetenv("DM_VK_ZCOPY"); put("driver-check.env", NULL); }

    put("boot_state", NULL); put("safe_start", NULL); put("gpu.txt", NULL); rmdir(D);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
