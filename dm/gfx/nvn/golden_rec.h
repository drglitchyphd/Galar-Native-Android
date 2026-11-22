// Golden-frame recorder (golden_rec.c): writes one frame as the NVN layer replays it into a .nxgold file (golden_fmt.h) for the PC player (dm/gfx/golden).
// Off unless DM_GOLDEN_REC is set; then:
//   DM_GOLDEN_REC=<present>[,<present>...]   record the frame that ends with each of those presents (counted from the start, as DM_VK_CAPTURE_AT counts them)
//   DM_GOLDEN_REC=req                        wait for golden.req in the game's folder (next to the pipeline cache, as capture.req): "<frames to wait> [label]"
//   DM_GOLDEN_DIR=<folder>                   where recordings go (default: <game's folder>/golden, else ./golden); never the repository
//   DM_GOLDEN_LABEL=<name>                   a label for the file names (title, overworld, battle, menu); golden.req's label wins
// Each recording is <title ID>-<label>-p<present>.nxgold, with <...>.phone.png: the picture this phone made of that frame.
// While off, the hooks below cost one load and a branch each.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "nvn_obj.h"
#include "golden_fmt.h"

extern int g_golden;   // -1 not looked at yet, 0 off, 1 armed (waiting for the frame), 2 recording this frame

// NVN layer (hle_nvn.c): around each submission's replay, and before each present reaches the replay layer
void golden_sub_begin(int n, const uint64_t *handles);
void golden_sub_end(void);
void golden_present(const tex_t *t, const int crop[4]);
#define GOLDEN_SUB_BEGIN(n, h) do { if (__builtin_expect(g_golden != 0, 0)) golden_sub_begin((n), (const uint64_t *)(h)); } while (0)
#define GOLDEN_SUB_END() do { if (__builtin_expect(g_golden == 2, 0)) golden_sub_end(); } while (0)
#define GOLDEN_PRESENT(t, crop) do { if (__builtin_expect(g_golden != 0, 0)) golden_present((t), (crop)); } while (0)

// Replay layer (nvn_vk.c): memory the GPU is about to read that the command records do not name (uniform / storage blocks, buffer ranges looked up by address)
void golden_mem_ref(uint64_t addr, uint64_t len);
#define GOLDEN_MEM(a, n) do { if (__builtin_expect(g_golden == 2, 0)) golden_mem_ref((uint64_t)(a), (uint64_t)(n)); } while (0)

// What the recorder needs from the backend that replays (golden_vk.inc for nvn_vk.c; a GLES backend brings its own). Weak: a build without them cannot record
typedef struct { uint64_t addr, size; } golden_be_pool_t;
int golden_be_pools(golden_be_pool_t *out, int max);                          // the memory pools now; returns how many (may exceed max)
int golden_be_pool_of(uint64_t addr, uint64_t *lo, uint64_t *hi);             // the pool holding addr (1), its bounds
int golden_be_treg_n(void);                                                   // texture registrations: table size, entry
int golden_be_treg_get(int id, tex_t *t, view_t *v, int *has_v);
int golden_be_sreg_n(void);
int golden_be_sreg_get(int id, samp_par *p);
void golden_be_globals(int *origin_ll, int *depth_zero, int *tex_pool_n, char *gpu, size_t ngpu);
size_t golden_be_state_ops(uint8_t *buf, size_t cap);                         // the replay state now, as command records ending in OP_END (bytes written; 0 if cap is short)
int golden_be_shader(const uint8_t key[8], int *stage, void **spv, size_t *nspv, void **vkr, size_t *nvkr, void **gl, size_t *ngl);   // malloc'd blobs, 1 found
void golden_be_frame_begin(void);                                             // the GPU idle; the images that exist now are the ones whose content counts
void golden_be_frame_end(void);
void golden_be_finish(void);                                                  // wait for everything submitted
int golden_be_shot(const tex_t *t, const int crop[4], const char *path);      // a picture of the texture (PNG), as the window would show it
// Player side: an image made for the texture (as its first use would) and filled with the recorded content; 1 done
int golden_be_tex_fill(const golden_texi *h, const uint8_t *data, size_t n);

// What the backend reports back while recording (its texture lookups: textures reach the replay through registrations, render targets, copies and handles in
// uniform buffers alike)
void golden_note_texi(const golden_texi *h, const uint8_t *data, size_t n);   // an image's content before this frame touched it
void golden_note_serial(uint64_t serial, int live, int cpu_written);          // a texture object the frame uses: mtl_tex_init'ed / CPU-filled on the phone
