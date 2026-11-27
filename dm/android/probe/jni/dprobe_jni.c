// The games' driver check (libdmprobe.so, DriverCheckService.java): the probe's self-tests (selftest.c) run inside the game's app, in a process of their own
//   (":drvcheck"), on the Vulkan driver the game will use. The app reads the journal as it grows; a test that takes this process down shows as a "begin" with no
//   result, and the app runs the check again without it. Nothing here touches the game's files except the journal and the human-readable report beside it.
#include <ctype.h>
#include <fcntl.h>
#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>

int dprobe_run(int journal_fd, const char *driver, const char *skip);   // selftest.c

static int g_txt = -1;   // the readable report (driver-check.txt): what the probe app would show
void probe_vout(const char *fmt, va_list ap) {
    char b[1024]; va_list a2; va_copy(a2, ap); int n = vsnprintf(b, sizeof b, fmt, a2); va_end(a2);
    if (n <= 0) return; if (n >= (int)sizeof b) n = sizeof b - 1;
    if (g_txt >= 0 && write(g_txt, b, (size_t)n) < 0) { }
    __android_log_print(ANDROID_LOG_INFO, "DriverCheck", "%s", b);
}

// The game's env.txt, as android_main.c env_file reads it (set only when not set): the driver's own switches (TU_DEBUG, PANVK_*) reach it here too
static void env_file(const char *path) {
    FILE *f = fopen(path, "r"); char ln[512]; if (!f) return;
    while (fgets(ln, sizeof ln, f)) { char *p = ln; while (*p == ' ' || *p == '\t') p++; if (*p == '#' || !*p) continue; char *eq = strchr(p, '='); if (!eq || eq == p) continue;
        *eq = 0; char *v = eq + 1; char *e = strpbrk(v, "\r\n"); if (e) *e = 0; if (!getenv(p)) setenv(p, v, 0); }
    fclose(f);
}
// android_main.c panvk_defaults: PanVK needs PANVK_ATOM_STRIDE=56 to start on Exynos 2100 / 1380
static void panvk_defaults(void) {
    const char *drv = getenv("DM_VK_DRIVER"); if (!drv || !strstr(drv, "panfrost")) return;
    char model[PROP_VALUE_MAX] = "", m[PROP_VALUE_MAX]; __system_property_get("ro.soc.model", model);
    size_t k = 0; for (; model[k] && k + 1 < sizeof m; k++) m[k] = (char)tolower((unsigned char)model[k]); m[k] = 0;
    if (strstr(m, "s5e9840") || strstr(m, "s5e8835") || strstr(m, "exynos2100") || strstr(m, "exynos1380")) setenv("PANVK_ATOM_STRIDE", "56", 0);
}

JNIEXPORT jint JNICALL Java_com_nxnative_DriverCheckService_run(JNIEnv *env, jclass c, jstring jjournal, jstring jtxt, jstring jenv, jstring jskip) {
    (void)c;
    const char *journal = (*env)->GetStringUTFChars(env, jjournal, NULL), *txt = (*env)->GetStringUTFChars(env, jtxt, NULL);
    const char *envf = (*env)->GetStringUTFChars(env, jenv, NULL), *skip = (*env)->GetStringUTFChars(env, jskip, NULL);
    env_file(envf); panvk_defaults();
    int fd = open(journal, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    g_txt = open(txt, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    const char *drv = getenv("DM_VK_DRIVER");
    int r = fd < 0 ? -1 : dprobe_run(fd, drv, skip);
    if (fd >= 0) close(fd); if (g_txt >= 0) { close(g_txt); g_txt = -1; }
    (*env)->ReleaseStringUTFChars(env, jjournal, journal); (*env)->ReleaseStringUTFChars(env, jtxt, txt);
    (*env)->ReleaseStringUTFChars(env, jenv, envf); (*env)->ReleaseStringUTFChars(env, jskip, skip);
    return r;
}
