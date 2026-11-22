// The SPIR-V side of DM_VK_B10_AS_RGBA16's stand-in (nvn_spv.c) on a module file: t_b10_spv <mode> in.spv out.spv [mask]
//   out  <mask>: spv_b10_outputs (the fragment outputs at the locations in mask made what R11G11B10F stores)
//   img:         spv_b10_image_writes, then spv_image_format_swap R11fG11fB10f -> Rgba16f (as nvn_vk.c shader_load does); "left N" on stdout
//   swap:        spv_image_format_swap alone; the count on stdout
// exit 0 and the module written when something was patched, 2 and nothing written when there was nothing to do
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "nvn_spv.h"
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: t_b10_spv out|img|swap in.spv out.spv [mask]\n"); return 1; }
    FILE *f = fopen(argv[2], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)n); if (fread(code, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
    size_t nw = (size_t)n / 4; uint32_t *o = NULL;
    if (!strcmp(argv[1], "out")) o = spv_b10_outputs(code, nw, argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 1u, &nw);
    else if (!strcmp(argv[1], "img")) { int left = 0; o = spv_b10_image_writes(code, nw, &nw, &left); printf("left %d\n", left);
        uint32_t *m = o ? o : code; int s = spv_image_format_swap(m, nw, SPV_IMGFMT_R11G11B10F, SPV_IMGFMT_RGBA16F); printf("swapped %d\n", s); if (!o && s) o = code; }
    else if (!strcmp(argv[1], "swap")) { int s = spv_image_format_swap(code, nw, SPV_IMGFMT_R11G11B10F, SPV_IMGFMT_RGBA16F); printf("swapped %d\n", s); if (s) o = code; }
    if (!o) return 2;
    f = fopen(argv[3], "wb"); fwrite(o, 4, nw, f); fclose(f);
    return 0;
}
