// probe_vk.h: the self-tests' Vulkan functions from the driver the game uses
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // (dladdr on glibc: the PC test harness)
#endif
#include "probe_vk.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define PV_DEF(f) PFN_##f pv_##f;
PV_PRE(PV_DEF) PV_INST(PV_DEF)
static PFN_vkGetInstanceProcAddr s_gipa;

static void *pv_open(const char *drv, const char **how) {
    if (drv && *drv) {
        char libdir[1024] = "", dir[1024]; Dl_info di;
        if (dladdr((void *)pv_open, &di) && di.dli_fname) { snprintf(libdir, sizeof libdir, "%s", di.dli_fname); char *e = strrchr(libdir, '/'); if (e) e[1] = 0; }   // the app's native library folder (the hook libraries)
        snprintf(dir, sizeof dir, "%s", drv); char *name = strrchr(dir, '/');
        void *at = dlopen("libadrenotools.so", RTLD_NOW);
        typedef void *(*open_fn)(int, int, const char *, const char *, const char *, const char *, const char *, void **);
        open_fn op = at ? (open_fn)dlsym(at, "adrenotools_open_libvulkan") : NULL;
        if (op && name) {
            char *lib = name + 1; *name = 0; char ddir[1030]; snprintf(ddir, sizeof ddir, "%s/", dir);
            void *h = op(RTLD_NOW, 1 /* ADRENOTOOLS_DRIVER_CUSTOM */, NULL, libdir, ddir, lib, NULL, NULL);
            if (h) { *how = "custom driver (libadrenotools)"; return h; }
        }
        void *h = dlopen(drv, RTLD_NOW | RTLD_LOCAL);
        if (h) { *how = "custom driver (opened directly)"; return h; }
        *how = "the system driver (the custom one could not be opened)";
    } else *how = "the system driver";
    void *h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return h ? h : dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);   // (desktop Linux: the PC test harness)
}
int pv_load(const char *driver, const char **how) {
    static const char *dummy; if (!how) how = &dummy;
    void *h = pv_open(driver, how); if (!h) return -1;
    s_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr"); if (!s_gipa) return -1;
#define PV_G(f) pv_##f = (PFN_##f)s_gipa(VK_NULL_HANDLE, #f);
    PV_PRE(PV_G)
#undef PV_G
    return pv_vkCreateInstance ? 0 : -1;
}
void pv_load_instance(VkInstance inst) {
#define PV_I(f) pv_##f = (PFN_##f)s_gipa(inst, #f);
    PV_INST(PV_I)
#undef PV_I
}
PFN_vkVoidFunction pv_instance_proc(VkInstance inst, const char *name) { return s_gipa ? s_gipa(inst, name) : NULL; }
