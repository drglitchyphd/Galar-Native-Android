// Golden-frame recordings (.nxgold): one frame of the game as the NVN layer handed it to the replay, self-contained, so a PC can replay it (dm/gfx/golden/golden_play.c)
// and compare the picture with an expected one (dm/gfx/golden/golden_check.py). Written by golden_rec.c, read by the player; see dm/gfx/golden/README.md.
//
// The format is the NVN layer's, not a backend's: command records exactly as hle_nvn.c wrote them (nvn_obj.h, followed through JUMP / CALL), the memory they read at the
// addresses they read it from, the texture / sampler registrations, the shaders by key, the replay state the frame starts from as NVN commands (PROL), and the images
// that hold pictures from earlier frames (TEXI). A GLES backend replays the same file through its own mtl_* functions.
//
// Layout: golden_file_hdr, then chunks { golden_chunk_hdr, payload (zlib when flags & GCF_ZLIB) } in replay order. Everything a chunk sets up (POOL, MEM, TREG, SREG,
// PROG, LIVE, CPUW, TEXI) applies before the next PROL / SUBM / PRES chunk. Little-endian, 64-bit pointers (arm64 phone, x86-64 PC). Addresses are the phone's: the
// player maps memory at the same addresses (39-bit user addresses on the phone fit x86-64's 47-bit user space).
#pragma once
#include <stdint.h>
#include "nvn_obj.h"

#define GOLDEN_MAGIC "NXGOLDN1"
#define GOLDEN_VERSION 1
#define GOLDEN_FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

typedef struct { char magic[8]; uint32_t version, hdr_size; } golden_file_hdr;            // hdr_size = sizeof(golden_file_hdr)
typedef struct { uint32_t type, flags; uint64_t raw, stored; } golden_chunk_hdr;            // raw: payload bytes after inflating; stored: bytes in the file
enum { GCF_ZLIB = 1 };

enum {
    GC_META = GOLDEN_FOURCC('M', 'E', 'T', 'A'),   // text, "key=value\n" lines: title, engine, present, label, gpu, driver, date, tex_t, origin_ll, depth_zero, tex_pool_n
    GC_POOL = GOLDEN_FOURCC('P', 'O', 'O', 'L'),   // golden_pool[]: memory pools (mtl_pool_add), in the order the replay had them
    GC_MEM  = GOLDEN_FOURCC('M', 'E', 'M', ' '),   // runs of { golden_mem_run, bytes[len] }: memory as the next submission reads it (pools, command memory, NVN objects)
    GC_TREG = GOLDEN_FOURCC('T', 'R', 'E', 'G'),   // golden_treg[]: texture descriptor registrations (mtl_reg_tex); ok 0 = unregistered
    GC_SREG = GOLDEN_FOURCC('S', 'R', 'E', 'G'),   // golden_sreg[]: sampler registrations (mtl_reg_samp)
    GC_SHDR = GOLDEN_FOURCC('S', 'H', 'D', 'R'),   // golden_shdr + SPIR-V (nspv) + reflection text (.vkr, nvkr) + GL-depth SPIR-V (ngl, may be 0)
    GC_PROG = GOLDEN_FOURCC('P', 'R', 'O', 'G'),   // golden_prog[]: a program serial's per-stage shader keys (prog_key)
    GC_LIVE = GOLDEN_FOURCC('L', 'I', 'V', 'E'),   // uint64_t serial[]: texture objects initialized and not finalized (mtl_tex_init)
    GC_CPUW = GOLDEN_FOURCC('C', 'P', 'U', 'W'),   // uint64_t serial[]: textures the CPU filled (mtl_tex_cpu_written): uploaded from memory, not GPU images
    GC_TEXI = GOLDEN_FOURCC('T', 'E', 'X', 'I'),   // golden_texi + texels: an image's content from earlier frames (render targets, copies), level by level
    GC_PROL = GOLDEN_FOURCC('P', 'R', 'O', 'L'),   // command records (nvn_obj.h) that set the replay state the frame starts from; replayed as a submission of their own
    GC_SUBM = GOLDEN_FOURCC('S', 'U', 'B', 'M'),   // golden_subm + uint64_t handle[n]: one queue submission (nvnQueueSubmitCommands)
    GC_PRES = GOLDEN_FOURCC('P', 'R', 'E', 'S'),   // golden_pres: the present that ends the frame: the picture to compare
    GC_END  = GOLDEN_FOURCC('E', 'N', 'D', ' '),
};

typedef struct { uint64_t addr, size; } golden_pool;
typedef struct { uint64_t addr, len; } golden_mem_run;                                       // followed by len bytes
typedef struct { int32_t id, ok, has_v, pad; tex_t t; view_t v; } golden_treg;
typedef struct { int32_t id, ok; samp_par p; } golden_sreg;
typedef struct { uint8_t key[8]; int32_t stage; uint32_t nspv, nvkr, ngl; } golden_shdr;    // stage: 0 vertex, 1 pixel, 2 geometry, 3 hull, 4 domain, 5 compute (shader_map.tsv's)
typedef struct { uint64_t serial; uint32_t nstage, has; uint8_t key[6][8]; } golden_prog;   // has: bit i = stage i's key known
typedef struct { uint32_t n, pad; } golden_subm;
typedef struct { uint64_t tex_addr; int32_t crop[4]; tex_t t; } golden_pres;
// An image's content, level by level; within a level every layer (3D: every slice of that level) in turn, rows tightly packed. kind 0: raw texels of format vkfmt
// (bpe bytes each); kind 1: depth as float32 in [0, 1] whatever the depth format, then, when has_stencil, the level's stencil bytes; kind 2: stencil bytes only. vkfmt is the recording backend's format (Vulkan's VkFormat);
// nvnfmt the texture's NVN format
typedef struct { tex_t t; int32_t vkfmt, nvnfmt; uint32_t w, h, d, levels, layers, samples, is3d, kind, has_stencil, bpe; } golden_texi;

_Static_assert(sizeof(golden_file_hdr) == 16 && sizeof(golden_chunk_hdr) == 24, "golden header layout");
