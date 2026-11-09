// FORK: the C interface of libforklsfg.so over lsfg-vk's framegen (LSFG_3_1 / LSFG_3_1P) and its shader extraction (Extract::). See fork_lsfg.h.
// lsfg-vk: https://github.com/FrankBarretta/lsfg-vk-android (Android branch of PancakeTAS/lsfg-vk), MIT. The model and shaders belong to Lossless Scaling (THS):
// they are read from the player's own Lossless.dll at run time and never stored in this repository or the APK.
#include "fork_lsfg.h"
#include "extract/extract.hpp"
#include "extract/trans.hpp"
#include "lsfg_3_1.hpp"
#include "lsfg_3_1p.hpp"
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

static std::string g_err;
static int g_perf;
static const char *set_err(const char *where, const std::exception &e) {
    g_err = std::string(where) + ": " + e.what();
    __android_log_print(ANDROID_LOG_WARN, "[FORK]", "[FORK] lsfg %s", g_err.c_str());
    return g_err.c_str();
}

extern "C" int fork_lsfg_init(const char *dll, uint64_t device_id, int perf, float flow_scale, int count) {
    try {
        setenv("LSFG_DLL_PATH_UNIX", dll, 1);   // (src/extract/extract.cpp: where Lossless.dll is)
        Extract::extractShaders();
        auto loader = [](const std::string &name) { return Extract::translateShader(Extract::getShader(name)); };
        g_perf = perf;
        if (flow_scale < 0.25f) flow_scale = 0.25f; if (flow_scale > 1.0f) flow_scale = 1.0f;
        if (perf) LSFG_3_1P::initialize(device_id, false, 1.0f / flow_scale, (uint64_t)count, loader);   // (lsfg-vk passes 1 / flow scale)
        else LSFG_3_1::initialize(device_id, false, 1.0f / flow_scale, (uint64_t)count, loader);
        return 0;
    } catch (const std::exception &e) { set_err("init", e); return -1; }
}
extern "C" int fork_lsfg_context(AHardwareBuffer *in0, AHardwareBuffer *in1, AHardwareBuffer *const *out, int nout, uint32_t w, uint32_t h, int vk_format) {
    try {
        std::vector<AHardwareBuffer *> o(out, out + nout);
        return g_perf ? LSFG_3_1P::createContextFromAHB(in0, in1, o, VkExtent2D{ w, h }, (VkFormat)vk_format)
                      : LSFG_3_1::createContextFromAHB(in0, in1, o, VkExtent2D{ w, h }, (VkFormat)vk_format);
    } catch (const std::exception &e) { set_err("context", e); return -1; }
}
extern "C" int fork_lsfg_present(int ctx) {
    try {
        std::vector<int> none;
        if (g_perf) { LSFG_3_1P::presentContext(ctx, -1, none); LSFG_3_1P::waitIdle(); }   // (no shared semaphores: done when this returns)
        else { LSFG_3_1::presentContext(ctx, -1, none); LSFG_3_1::waitIdle(); }
        return 0;
    } catch (const std::exception &e) { set_err("present", e); return -1; }
}
extern "C" void fork_lsfg_delete(int ctx) { try { if (g_perf) LSFG_3_1P::deleteContext(ctx); else LSFG_3_1::deleteContext(ctx); } catch (const std::exception &e) { set_err("delete", e); } }
extern "C" void fork_lsfg_finish(void) { try { if (g_perf) LSFG_3_1P::finalize(); else LSFG_3_1::finalize(); } catch (const std::exception &e) { set_err("finish", e); } }
extern "C" const char *fork_lsfg_error(void) { return g_err.c_str(); }
