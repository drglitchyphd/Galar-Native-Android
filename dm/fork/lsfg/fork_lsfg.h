// FORK: libforklsfg.so, frame generation for the fork (Lossless Scaling's LSFG 3.1 through lsfg-vk, MIT). A C interface the engine loads with dlopen
// (gfx/nvn/fork_fg.h), so the engine stays C and runs without it. The shaders come from the player's own Lossless.dll (never shipped): extracted and
// translated to SPIR-V on the phone at the first initialisation. Every call returns 0 / an id on success and a negative value on failure (fork_lsfg_error says why).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct AHardwareBuffer;
// dll: the player's Lossless.dll. device_id: the GPU (vendorID << 32 | deviceID, as the engine's own device). perf: the cheaper "performance" model.
// flow_scale 0.25-1 (lower is faster, rougher). count: generated frames per real frame (1 = 2x)
int fork_lsfg_init(const char *dll, uint64_t device_id, int perf, float flow_scale, int count);
// the two input images (the last two real frames, alternately written) and the outputs, all w x h, Vulkan format vk_format
int fork_lsfg_context(struct AHardwareBuffer *in0, struct AHardwareBuffer *in1, struct AHardwareBuffer *const *out, int nout, uint32_t w, uint32_t h, int vk_format);
// the newest real frame is in input (frame number % 2): generate the in-between frame(s) into the outputs and wait until they are done
int fork_lsfg_present(int ctx);
void fork_lsfg_delete(int ctx);
void fork_lsfg_finish(void);
const char *fork_lsfg_error(void);
#ifdef __cplusplus
}
#endif
