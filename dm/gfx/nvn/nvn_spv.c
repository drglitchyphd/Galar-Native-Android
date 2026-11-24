// SPIR-V reading for the Vulkan replay, see nvn_spv.h. Only what the translator's shaders use: names, Binding / DescriptorSet / Location / Block / BufferBlock decorations,
// variables in UniformConstant / Uniform / StorageBuffer / Input storage, and the types behind them.
#include "nvn_spv.h"
#include <stdlib.h>
#include <string.h>

enum { OpName = 5, OpEntryPoint = 15, OpExecutionMode = 16, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27,
       OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpDecorate = 71 };
enum { DecBlock = 2, DecBufferBlock = 3, DecBuiltIn = 11, DecLocation = 30, DecBinding = 33, DecDescriptorSet = 34 };
enum { SC_UniformConstant = 0, SC_Input = 1, SC_Uniform = 2, SC_StorageBuffer = 12 };

typedef struct { uint16_t op; uint32_t a, b, c, d, e, f, g; uint8_t block, bufblock, has_binding, has_set, has_loc, builtin; uint32_t binding, set, loc; char name[48]; } ent;

int spv_reflect(const uint32_t *code, size_t words, spv_res *res, int max_res, int *nres, spv_input *in, int max_in, int *nin, int *stage, uint32_t ls[3]) {
    *nres = 0; if (nin) *nin = 0; *stage = -1; if (ls) ls[0] = ls[1] = ls[2] = 0;
    if (words < 5 || code[0] != 0x07230203u) return -1;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return -1;
    ent *e = calloc(bound, sizeof *e); if (!e) return -1;
    uint32_t *vars = malloc(sizeof(uint32_t) * bound), nv = 0;
    for (size_t i = 5; i < words;) {
        uint32_t w = code[i], n = w >> 16, op = w & 0xffff;
        if (!n || i + n > words) break;
        const uint32_t *x = code + i;
        if (op == OpName && n >= 3 && x[1] < bound) { size_t m = (n - 2) * 4; if (m > sizeof e[0].name - 1) m = sizeof e[0].name - 1; memcpy(e[x[1]].name, x + 2, m); e[x[1]].name[m] = 0; }
        else if (op == OpEntryPoint && n >= 3) *stage = (int)x[1];
        else if (op == OpExecutionMode && n >= 6 && x[2] == 17 && ls) { ls[0] = x[3]; ls[1] = x[4]; ls[2] = x[5]; }
        else if (op == OpDecorate && n >= 3 && x[1] < bound) {
            ent *t = &e[x[1]];
            switch (x[2]) {
                case DecBlock: t->block = 1; break; case DecBufferBlock: t->bufblock = 1; break; case DecBuiltIn: t->builtin = 1; break;
                case DecBinding: if (n >= 4) { t->has_binding = 1; t->binding = x[3]; } break;
                case DecDescriptorSet: if (n >= 4) { t->has_set = 1; t->set = x[3]; } break;
                case DecLocation: if (n >= 4) { t->has_loc = 1; t->loc = x[3]; } break;
            }
        }
        else if ((op == OpTypeInt || op == OpTypeFloat || op == OpTypeVector || op == OpTypeImage || op == OpTypeSampler || op == OpTypeSampledImage || op == OpTypeArray
                  || op == OpTypeRuntimeArray || op == OpTypeStruct || op == OpTypePointer) && n >= 2 && x[1] < bound) {
            ent *t = &e[x[1]]; t->op = (uint16_t)op;
            if (n > 2) t->a = x[2]; if (n > 3) t->b = x[3]; if (n > 4) t->c = x[4]; if (n > 5) t->d = x[5]; if (n > 6) t->e = x[6]; if (n > 7) t->f = x[7]; if (n > 8) t->g = x[8];
        }
        else if (op == OpConstant && n >= 4 && x[2] < bound) { e[x[2]].op = OpConstant; e[x[2]].a = x[1]; e[x[2]].b = x[3]; }   // result type first, then the result: the value is in b
        else if (op == OpVariable && n >= 4 && x[2] < bound) { e[x[2]].op = OpVariable; e[x[2]].a = x[1]; e[x[2]].b = x[3]; vars[nv++] = x[2]; }   // a = pointer type, b = storage class
        i += n;
    }
    for (uint32_t k = 0; k < nv; k++) {
        uint32_t id = vars[k]; ent *v = &e[id]; uint32_t sc = v->b;
        if (v->a >= bound || e[v->a].op != OpTypePointer) continue;
        uint32_t ty = e[v->a].b;
        if (sc == SC_Input && v->has_loc && !v->builtin && in && nin && *nin < max_in) {   // vertex inputs: component class and count of the (vector) type
            ent *t = ty < bound ? &e[ty] : NULL; uint8_t nc = 1;
            if (t && t->op == OpTypeVector) { nc = (uint8_t)t->b; t = t->a < bound ? &e[t->a] : NULL; }
            in[*nin].location = v->loc; in[*nin].ncomp = nc;
            in[*nin].sclass = t && t->op == OpTypeInt ? (t->b ? 2 : 1) : 0; (*nin)++;
            continue;
        }
        if (sc != SC_UniformConstant && sc != SC_Uniform && sc != SC_StorageBuffer) continue;
        if (!v->has_binding || *nres >= max_res) continue;
        spv_res *r = &res[*nres]; memset(r, 0, sizeof *r);
        strncpy(r->name, v->name, sizeof r->name - 1); r->set = v->set; r->binding = v->binding; r->count = 1;
        ent *t = ty < bound ? &e[ty] : NULL;
        if (t && (t->op == OpTypeArray || t->op == OpTypeRuntimeArray)) {
            if (t->op == OpTypeArray && t->b < bound && e[t->b].op == OpConstant) r->count = e[t->b].b;
            t = t->a < bound ? &e[t->a] : NULL;
        }
        if (!t) continue;
        if (sc == SC_StorageBuffer || (sc == SC_Uniform && t->bufblock)) r->kind = SPV_RES_SSBO;
        else if (sc == SC_Uniform) r->kind = SPV_RES_UBO;
        else {
            ent *im = t;
            if (t->op == OpTypeSampledImage) im = t->a < bound ? &e[t->a] : NULL;
            if (t->op == OpTypeSampler) r->kind = SPV_RES_SAMP;
            else if (im && im->op == OpTypeImage) {
                // OpTypeImage: a = sampled type, b = dim, c = depth, d = arrayed, e = ms, f = sampled (1 with a sampler, 2 storage), g = image format
                r->dim = (uint8_t)im->b; r->depth = im->c == 1; r->arrayed = (uint8_t)im->d; r->ms = (uint8_t)im->e; r->ifmt = (uint8_t)(im->g < 256 ? im->g : 0);
                ent *st = im->a < bound ? &e[im->a] : NULL; r->sclass = st && st->op == OpTypeInt ? (st->b ? 2 : 1) : 0;
                if (im->f == 2) r->kind = im->b == SPV_DIM_BUFFER ? SPV_RES_IBUF : SPV_RES_IMG;
                else if (im->b == SPV_DIM_BUFFER) r->kind = SPV_RES_TBUF;
                else r->kind = t->op == OpTypeSampledImage ? SPV_RES_TEX : SPV_RES_SIMG;
            } else continue;
        }
        (*nres)++;
    }
    free(vars); free(e);
    return *stage < 0 ? -1 : 0;
}

void spv_rebind(uint32_t *code, size_t words, uint32_t add) {
    for (size_t i = 5; i < words;) {
        uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (!n || i + n > words) return;
        if (op == OpDecorate && n >= 4 && code[i + 2] == DecBinding) code[i + 3] += add;
        i += n;
    }
}

enum { OpTypeBool = 20, OpTypeMatrix = 24, OpConstantNull = 46, OpAccessChain = 65, OpInBoundsAccessChain = 66, SC_Output = 3, SC_Private = 6 };
static uint32_t slots_of(const uint32_t *code, size_t words, uint32_t ty, int depth);
// The definition of id (its instruction's start), by a scan: used only for the few types behind I/O variables
static const uint32_t *def_of(const uint32_t *code, size_t words, uint32_t id) {
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return NULL;
        if (((op >= OpTypeBool && op <= OpTypePointer) || op == OpConstant) && n >= 2 && code[i + (op == OpConstant ? 2 : 1)] == id) return code + i;
        i += n; }
    return NULL;
}
static uint32_t slots_of(const uint32_t *code, size_t words, uint32_t ty, int depth) {
    const uint32_t *d = def_of(code, words, ty); if (!d || depth > 4) return 1;
    uint32_t op = d[0] & 0xffff;
    if (op == OpTypeArray) { const uint32_t *c = def_of(code, words, d[3]); uint32_t len = c && (c[0] & 0xffff) == OpConstant ? c[3] : 1; return len * slots_of(code, words, d[2], depth + 1); }
    if (op == OpTypeMatrix) return d[3];
    return 1;
}
void spv_io_masks(const uint32_t *code, size_t words, uint64_t *in_mask, uint64_t *out_mask) {
    *in_mask = *out_mask = 0;
    if (words < 5 || code[0] != 0x07230203u) return;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return;
    int32_t *loc = malloc(sizeof(int32_t) * bound); uint8_t *bi = calloc(bound, 1); for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpDecorate && n >= 3 && x[1] < bound) { if (x[2] == DecLocation && n >= 4) loc[x[1]] = (int32_t)x[3]; else if (x[2] == DecBuiltIn) bi[x[1]] = 1; }
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[2] < bound && (x[3] == SC_Input || x[3] == SC_Output) && loc[x[2]] >= 0 && !bi[x[2]]) {
            const uint32_t *p = def_of(code, words, x[1]); uint32_t k = p && (p[0] & 0xffff) == OpTypePointer ? slots_of(code, words, p[3], 0) : 1;
            for (uint32_t s = 0; s < k && loc[x[2]] + s < 64; s++) *(x[3] == SC_Input ? in_mask : out_mask) |= 1ull << (loc[x[2]] + s);
        }
        i += n; }
    free(loc); free(bi);
}
// Emits (in the types / constants section) the value an unwritten Maxwell attribute reads, for a varying of type ty: (0, 0, 0, 1) for a 4-component vector (the 1 as the
// bits of float 1.0 whatever the component type: attributes are untyped 32-bit slots), zeros otherwise. Returns its id (new ids from *nb)
static uint32_t emit_default(uint32_t *o, size_t *w, const uint32_t *code, size_t words, uint32_t ty, uint32_t *nb) {
    const uint32_t *d = def_of(code, words, ty);
    if (d && (d[0] & 0xffff) == OpTypeVector && d[3] >= 2 && d[3] <= 4) {
        const uint32_t *c = def_of(code, words, d[2]);
        if (c && ((c[0] & 0xffff) == OpTypeFloat || (c[0] & 0xffff) == OpTypeInt) && c[2] == 32) {
            uint32_t z = (*nb)++, one = (*nb)++, v = (*nb)++;
            o[(*w)++] = 4u << 16 | OpConstant; o[(*w)++] = d[2]; o[(*w)++] = z; o[(*w)++] = 0;
            o[(*w)++] = 4u << 16 | OpConstant; o[(*w)++] = d[2]; o[(*w)++] = one; o[(*w)++] = 0x3f800000u;
            o[(*w)++] = (3 + d[3]) << 16 | 44 /* OpConstantComposite */; o[(*w)++] = ty; o[(*w)++] = v;
            for (uint32_t k = 0; k < d[3]; k++) o[(*w)++] = k == 3 ? one : z;
            return v;
        }
    }
    uint32_t v = (*nb)++; o[(*w)++] = 3u << 16 | OpConstantNull; o[(*w)++] = ty; o[(*w)++] = v; return v;
}
uint32_t *spv_drop_inputs(const uint32_t *code, size_t words, uint64_t drop, size_t *out_words) {
    if (!drop || words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    int32_t *loc = malloc(sizeof(int32_t) * bound); uint8_t *bi = calloc(bound, 1), *dv = calloc(bound, 1); uint32_t *pp = calloc(bound, 4), *pn = calloc(bound, 4);
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpDecorate && n >= 3 && x[1] < bound) { if (x[2] == DecLocation && n >= 4) loc[x[1]] = (int32_t)x[3]; else if (x[2] == DecBuiltIn) bi[x[1]] = 1; }
        i += n; }
    int any = 0;   // dv: dropped variables; pp[input pointer type] = its Private twin (made below), pn[pointee] = OpConstantNull of it
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[2] < bound && x[1] < bound && x[3] == SC_Input && loc[x[2]] >= 0 && loc[x[2]] < 64 && !bi[x[2]] && (drop >> loc[x[2]] & 1)) { dv[x[2]] = 1; pp[x[1]] = 1; any = 1; }
        else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4 && x[1] < bound && x[2] < bound && x[3] < bound && dv[x[3]]) { dv[x[2]] = 2; pp[x[1]] = 1; }   // pointers into them
        i += n; }
    if (!any) { free(loc); free(bi); free(dv); free(pp); free(pn); return NULL; }
    uint32_t *o = malloc(sizeof(uint32_t) * (words + 64 + 8 * bound / 64 + 1024)); size_t w = 5, cap = words + 64 + 8 * bound / 64 + 1024; memcpy(o, code, 20);
    uint32_t nb = bound; int pre14 = code[1] < 0x00010400u;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (w + n + 16 > cap) { cap = cap * 2 + n; o = realloc(o, sizeof(uint32_t) * cap); }
        if (op == OpDecorate && n >= 2 && x[1] < bound && dv[x[1]] == 1) { i += n; continue; }   // no Location / Flat / … on a Private variable
        if (op == OpEntryPoint && pre14) {                               // before SPIR-V 1.4 the interface lists only Input / Output
            size_t s = 3; while (s < n && (x[s] & 0xff000000u) && (x[s] & 0xff0000u) && (x[s] & 0xff00u) && (x[s] & 0xffu)) s++; s++;   // past the name's last word (the one with a zero byte)
            size_t at = w; memcpy(o + w, x, 4 * s); w += s;
            for (size_t k = s; k < n; k++) if (!(x[k] < bound && dv[x[k]] == 1)) o[w++] = x[k];
            o[at] = (uint32_t)(w - at) << 16 | op; i += n; continue;
        }
        if (op == OpTypePointer && n >= 4 && x[1] < bound && pp[x[1]] == 1) {   // the Input pointer type: then its Private twin and the null constant of the pointee
            memcpy(o + w, x, 4 * n); w += n;
            uint32_t tw = nb++; o[w++] = 4u << 16 | OpTypePointer; o[w++] = tw; o[w++] = SC_Private; o[w++] = x[3]; pp[x[1]] = tw;
            if (x[3] < bound && !pn[x[3]]) pn[x[3]] = emit_default(o, &w, code, words, x[3], &nb);
            i += n; continue;
        }
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4 && x[2] < bound && dv[x[2]] == 2 && x[1] < bound && pp[x[1]] > 1) {
            memcpy(o + w, x, 4 * n); o[w + 1] = pp[x[1]]; w += n; i += n; continue; }
        if (op == OpVariable && n >= 4 && x[2] < bound && dv[x[2]] == 1) {
            const uint32_t *p = def_of(code, words, x[1]); uint32_t pointee = p ? p[3] : 0;
            o[w++] = 5u << 16 | OpVariable; o[w++] = pp[x[1]]; o[w++] = x[2]; o[w++] = SC_Private; o[w++] = pointee < bound ? pn[pointee] : 0;
            i += n; continue;
        }
        memcpy(o + w, x, 4 * n); w += n; i += n;
    }
    o[3] = nb; *out_words = w;
    free(loc); free(bi); free(dv); free(pp); free(pn);
    return o;
}

// Dual-source blending without dualSrcBlend (nvn_vk.c draw): a copy whose outputs at locations 0 and 1 trade places, so the second output reaches the colour
//   target and the blend can use it as the source. NULL: not a SPIR-V module or no output at location 1
uint32_t *spv_swap_out01(const uint32_t *code, size_t words, size_t *out_words) {
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint8_t *out = calloc(bound, 1); int has1 = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[2] < bound && x[3] == SC_Output) out[x[2]] = 1;
        i += n; }
    uint32_t *o = malloc(sizeof(uint32_t) * words); memcpy(o, code, sizeof(uint32_t) * words);
    for (size_t i = 5; i < words;) { uint32_t n = o[i] >> 16, op = o[i] & 0xffff; if (!n || i + n > words) break; uint32_t *x = o + i;
        if (op == OpDecorate && n >= 4 && x[1] < bound && out[x[1]] && x[2] == DecLocation && x[3] <= 1) { if (x[3] == 1) has1 = 1; x[3] ^= 1; }
        i += n; }
    free(out);
    if (!has1) { free(o); return NULL; }
    *out_words = words; return o;
}

uint32_t *spv_init_outputs(const uint32_t *code, size_t words, size_t *out_words) {
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint8_t *ov = calloc(bound, 1), *bi = calloc(bound, 1), *hasloc = calloc(bound, 1); uint32_t *pt = calloc(bound, 4), *dc = calloc(bound, 4); uint32_t entry = 0; int any = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpDecorate && n >= 3 && x[1] < bound) { if (x[2] == DecLocation) hasloc[x[1]] = 1; else if (x[2] == DecBuiltIn) bi[x[1]] = 1; }
        else if (op == OpEntryPoint && n >= 3 && !entry) entry = x[2];
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[2] < bound && x[1] < bound && x[3] == SC_Output && hasloc[x[2]] && !bi[x[2]]) {
            const uint32_t *p = def_of(code, words, x[1]); const uint32_t *d = p ? def_of(code, words, p[3]) : NULL;
            if (d && (d[0] & 0xffff) == OpTypeVector) { ov[x[2]] = 1; pt[x[1]] = 1; any = 1; } }   // vectors only (arrays of them are rare in vertex outputs)
        i += n; }
    if (!any || !entry) { free(ov); free(bi); free(hasloc); free(pt); free(dc); return NULL; }
    size_t cap = words + 64 + 16 * 64; uint32_t *o = malloc(sizeof(uint32_t) * cap), nb = bound; size_t w = 5; memcpy(o, code, 20);
    int in_entry = 0, stored = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (w + n + 64 > cap) { cap = cap * 2 + n; o = realloc(o, sizeof(uint32_t) * cap); }
        if (op == OpTypePointer && n >= 4 && x[1] < bound && pt[x[1]] == 1) {
            memcpy(o + w, x, 4 * n); w += n; if (x[3] < bound && !dc[x[3]]) dc[x[3]] = emit_default(o, &w, code, words, x[3], &nb); pt[x[1]] = 2; i += n; continue; }
        if (op == 54 /* OpFunction */ && n >= 3 && x[2] == entry) in_entry = 1;
        // in the entry function's first block, after its OpVariables: store the default into every user vector output
        if (in_entry && !stored && op != 248 /* OpLabel */ && op != OpVariable && op != 54 && op != 55 /* OpFunctionParameter */ && op != 8 /* OpLine */ && op != 317 /* OpNoLine */) {
            for (size_t k = 5; k < words;) { uint32_t kn = code[k] >> 16, kop = code[k] & 0xffff; if (!kn || k + kn > words) break; const uint32_t *y = code + k;
                if (kop == OpVariable && kn >= 4 && y[2] < bound && ov[y[2]]) { const uint32_t *p = def_of(code, words, y[1]);
                    if (p && p[3] < bound && dc[p[3]]) { if (w + 4 > cap) { cap *= 2; o = realloc(o, sizeof(uint32_t) * cap); } o[w++] = 3u << 16 | 62 /* OpStore */; o[w++] = y[2]; o[w++] = dc[p[3]]; } }
                k += kn; }
            stored = 1;
        }
        memcpy(o + w, x, 4 * n); w += n; i += n;
    }
    o[3] = nb; *out_words = w;
    free(ov); free(bi); free(hasloc); free(pt); free(dc);
    if (!stored) { free(o); return NULL; }
    return o;
}

// ---- Clean-up for strict drivers (Adreno): the translator declares capabilities every shader of a stage gets, whether it uses them or not ----
// Also (Vulkan 1.0 baseline, every device): the subgroup capabilities (Vote / Ballot / Shuffle: every translated shader declares them, none of 77,002 Sword / Shield
//   shaders uses one; a Vulkan 1.1 driver need only support them in the stages its subgroupSupportedStages lists, Adreno 6xx not the vertex stage) and ImageGatherExtended
//   (declared by every shader; needs shaderImageGatherExtended, optional in Vulkan 1.0) where nothing uses a non-constant offset
static int spv_img_operand_at(uint32_t op) {   // the image-operands word of an image instruction (0: none / not an image instruction)
    switch (op) { case 87: case 88: case 91: case 92: case 95: case 98: return 5; case 89: case 90: case 93: case 94: case 96: case 97: return 6; case 99: return 4; default: return 0; }
}
size_t spv_strip_unused(uint32_t *code, size_t words) {
    if (words < 5 || code[0] != 0x07230203u) return words;
    int f64 = 0, ilk = 0, sg = 0, gext = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return words; const uint32_t *x = code + i;
        if (op == OpTypeFloat && n >= 3 && code[i + 2] == 64) f64 = 1;
        if (op == 5364 || op == 5365) ilk = 1;   // OpBegin / OpEndInvocationInterlockEXT
        if ((op >= 333 && op <= 366) || op == 4421 || op == 4422 || (op >= 4428 && op <= 4430) || op == 4432) sg = 1;   // OpGroupNonUniform*, the KHR ballot / vote instructions
        if (op == OpDecorate && n >= 4 && x[2] == DecBuiltIn && ((x[3] >= 36 && x[3] <= 41) || (x[3] >= 4416 && x[3] <= 4420))) sg = 1;   // subgroup built-ins
        { int at = spv_img_operand_at(op); if (at && n > (uint32_t)at && (x[at] & 0x30)) gext = 1; }   // Offset / ConstOffsets
        if (op >= 305 && op <= 320) gext = 1;   // sparse image instructions: left as declared
        i += n; }
    size_t w = 5;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i; int drop = 0;
        if (op == 17 /* OpCapability */ && n == 2 && ((x[1] == 10 && !f64) || ((x[1] == 5378 || x[1] == 5363 || x[1] == 5372) && !ilk))) drop = 1;
        if (op == 17 && n == 2 && !sg && ((x[1] >= 61 && x[1] <= 66) || x[1] == 4423 || x[1] == 4431)) drop = 1;
        if (op == 17 && n == 2 && !gext && x[1] == 25) drop = 1;
        if (op == 10 && !sg && n >= 2 && (!strncmp((const char *)(x + 1), "SPV_KHR_shader_ballot", 4 * (n - 1)) || !strncmp((const char *)(x + 1), "SPV_KHR_subgroup_vote", 4 * (n - 1)))) drop = 1;
        if (op == 10 /* OpExtension */ && !ilk && n >= 2 && !strncmp((const char *)(x + 1), "SPV_EXT_fragment_shader_interlock", 4 * (n - 1))) drop = 1;
        if (op == OpExecutionMode && n >= 3 && x[2] >= 5366 && x[2] <= 5371 && !ilk) drop = 1;   // the interlock execution modes
        if (!drop) { memmove(code + w, x, 4 * n); w += n; }
        i += n; }
    return w;
}

// Unused ClipDistance / CullDistance members of the per-vertex block, dropped. The translator declares every vertex shader's gl_PerVertex with ClipDistance[8] and
//   CullDistance[1], written by none of them (Shield's 18,300): 9 is over the 8 of maxCombinedClipAndCullDistances most GPUs have (VUID-RuntimeSpirv-00710), and the
//   CullDistance capability needs shaderCullDistance, which some drivers lack. Only when they are the block's last members and nothing reaches them: every use of a
//   variable of the block is an access chain whose first index is a constant naming another member (else the shader is left as it is). Their capabilities go
//   when no ClipDistance / CullDistance is left. In place; the new word count
size_t spv_strip_clipcull(uint32_t *code, size_t words) {
    enum { OpMemberName = 6, OpFunctionCall = 57, OpLoad = 61, OpStore = 62, OpCopyMemory = 63, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpPtrAccessChain = 67,
           OpMemberDecorate = 72, OpCopyObject = 83, OpSelect = 169, OpPhi = 245, MAXS = 4 };
    if (words < 5 || code[0] != 0x07230203u) return words;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return words;
    uint32_t sid[MAXS], smask[MAXS], skeep[MAXS], sbits[MAXS][2]; int ns = 0, var_clip = 0, var_cull = 0;   // sbits: the members that are ClipDistance / CullDistance
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return words; const uint32_t *x = code + i;
        if (op == OpMemberDecorate && n >= 5 && x[3] == DecBuiltIn && (x[4] == 3 || x[4] == 4)) {
            if (x[2] >= 32) return words;
            int k = 0; while (k < ns && sid[k] != x[1]) k++;
            if (k == ns) { if (ns == MAXS) return words; sid[ns] = x[1]; smask[ns] = sbits[ns][0] = sbits[ns][1] = 0; skeep[ns] = 32; ns++; }
            smask[k] |= 1u << x[2]; sbits[k][x[4] - 3] |= 1u << x[2]; }
        else if (op == OpDecorate && n >= 4 && x[2] == DecBuiltIn) { if (x[3] == 3) var_clip = 1; else if (x[3] == 4) var_cull = 1; }
        i += n; }
    if (!ns) return words;
    uint8_t *of = calloc(bound, 1); uint32_t *cv = calloc(bound, 4);   // of: 1 + the struct's index for its pointer types and variables (bit 7: a variable); cv: constants + 1
    if (!of || !cv) { free(of); free(cv); return words; }
    int ok = 1, seen = 0;
    for (size_t i = 5; i < words && ok;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpTypeStruct && n >= 2) {
            for (int k = 0; k < ns; k++) if (sid[k] == x[1]) {   // the dropped members must be the last ones
                uint32_t cnt = n - 2, lo = 0; while (lo < cnt && !(smask[k] >> lo & 1)) lo++;
                if (cnt > 32 || !lo || lo == cnt || smask[k] != (uint32_t)(((uint64_t)1 << cnt) - ((uint64_t)1 << lo))) ok = 0;
                skeep[k] = lo; seen++; }
            for (uint32_t m = 2; m < n; m++) for (int k = 0; k < ns; k++) if (x[m] == sid[k]) ok = 0;   // (nested in another struct)
        }
        else if ((op == OpTypeArray || op == OpTypeRuntimeArray) && n >= 3) { for (int k = 0; k < ns; k++) if (x[2] == sid[k]) ok = 0; }   // (arrays of it: geometry / tessellation)
        else if (op == OpTypePointer && n >= 4 && x[1] < bound) { for (int k = 0; k < ns; k++) if (x[3] == sid[k]) of[x[1]] = (uint8_t)(1 + k); }
        else if (op == OpConstant && n == 4 && x[2] < bound) cv[x[2]] = x[3] + 1;
        else if (op == OpVariable && n >= 4 && x[1] < bound && x[2] < bound && of[x[1]]) of[x[2]] = (uint8_t)(of[x[1]] | 0x80);
        else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4 && x[3] < bound && (of[x[3]] & 0x80)) {
            int k = (of[x[3]] & 0x7f) - 1;
            if (n < 5 || x[4] >= bound || !cv[x[4]] || cv[x[4]] - 1 >= skeep[k]) ok = 0; }
        else {   // any other use of such a variable: left as it is
            uint32_t a = 0, b = 0;
            switch (op) { case OpLoad: case OpCopyObject: case OpPtrAccessChain: a = 3; b = 4; break; case OpStore: a = 1; b = 2; break; case OpCopyMemory: a = 1; b = 3; break;
                          case OpFunctionCall: case OpSelect: a = 4; b = n; break; case OpPhi: a = 3; b = n; break; default: break; }
            for (uint32_t m = a; m < b && m < n; m++) if (x[m] < bound && (of[x[m]] & 0x80)) ok = 0;
        }
        i += n; }
    free(of); free(cv);
    if (!ok || seen != ns) return words;
    int left_clip = var_clip, left_cull = var_cull;
    for (int k = 0; k < ns; k++) { uint32_t keep = (1u << skeep[k]) - 1; if (sbits[k][0] & keep) left_clip = 1; if (sbits[k][1] & keep) left_cull = 1; }
    size_t w = 5;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i; int drop = 0; uint32_t nn = n;
        int k = -1; if ((op == OpTypeStruct || op == OpMemberDecorate || op == OpMemberName) && n >= 2) for (int j = 0; j < ns; j++) if (sid[j] == x[1]) k = j;
        if (k >= 0 && op == OpTypeStruct) nn = 2 + skeep[k];
        else if (k >= 0 && n >= 3 && x[2] >= skeep[k]) drop = 1;   // (their member decorations and names)
        if (op == 17 /* OpCapability */ && n == 2 && ((x[1] == 32 && !left_clip) || (x[1] == 33 && !left_cull))) drop = 1;
        if (!drop) { memmove(code + w, x, 4 * nn); code[w] = nn << 16 | op; w += nn; }
        i += n; }
    return w;
}

// The component class of each fragment output location: 0 float, 1 uint, 2 sint, 255 none
void spv_frag_outputs(const uint32_t *code, size_t words, uint8_t cls[8]) {
    memset(cls, 255, 8);
    if (words < 5 || code[0] != 0x07230203u) return;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return;
    int32_t *loc = malloc(sizeof(int32_t) * bound); for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break;
        if (op == OpDecorate && n >= 4 && code[i + 1] < bound && code[i + 2] == DecLocation) loc[code[i + 1]] = (int32_t)code[i + 3];
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[3] == SC_Output && x[2] < bound && loc[x[2]] >= 0 && loc[x[2]] < 8) {
            const uint32_t *p = def_of(code, words, x[1]); const uint32_t *t = p ? def_of(code, words, p[3]) : NULL;
            if (t && (t[0] & 0xffff) == OpTypeVector) t = def_of(code, words, t[2]);
            if (t) cls[loc[x[2]]] = (t[0] & 0xffff) == OpTypeInt ? (t[3] ? 2 : 1) : 0; }
        i += n; }
    free(loc);
}

// A copy in which the float outputs at the locations in umask / smask become uint / int outputs holding the same bits (OpBitcast before each store), for a colour
// target of an integer format: Maxwell's outputs are untyped registers the ROP writes as they are. NULL if the outputs cannot be rewritten (an output read back)
uint32_t *spv_int_outputs(const uint32_t *code, size_t words, uint32_t umask, uint32_t smask, size_t *out_words) {
    if (!(umask | smask) || words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    int32_t *loc = malloc(sizeof(int32_t) * bound); uint8_t *tv = calloc(bound, 1); uint32_t *ncomp = calloc(bound, 4); for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    uint32_t id_int[2] = {0, 0}, id_vec[2][5] = {{0}}, id_pv[2][5] = {{0}}, id_ps[2] = {0, 0};   // existing types: int scalar, vectors, Output pointers to them
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) { free(loc); free(tv); free(ncomp); return NULL; } const uint32_t *x = code + i;
        if (op == OpDecorate && n >= 4 && x[1] < bound && x[2] == DecLocation) loc[x[1]] = (int32_t)x[3];
        if (op == OpTypeInt && n >= 4 && x[2] == 32) id_int[x[3] ? 1 : 0] = x[1];
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpTypeVector && n >= 4 && x[3] <= 4) for (int s = 0; s < 2; s++) if (id_int[s] && x[2] == id_int[s]) id_vec[s][x[3]] = x[1];
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpTypePointer && n >= 4 && x[2] == SC_Output) for (int s = 0; s < 2; s++) { if (id_int[s] && x[3] == id_int[s]) id_ps[s] = x[1]; for (int c = 2; c <= 4; c++) if (id_vec[s][c] && x[3] == id_vec[s][c]) id_pv[s][c] = x[1]; }
        if (op == OpVariable && n >= 4 && x[3] == SC_Output && x[2] < bound && loc[x[2]] >= 0 && loc[x[2]] < 8) {
            uint32_t bit = 1u << loc[x[2]]; int s = (smask & bit) ? 1 : (umask & bit) ? 0 : -1;
            const uint32_t *p = def_of(code, words, x[1]); const uint32_t *t = p ? def_of(code, words, p[3]) : NULL; uint32_t nc = 1; const uint32_t *ct = t;
            if (t && (t[0] & 0xffff) == OpTypeVector) { nc = t[3]; ct = def_of(code, words, t[2]); }
            if (s >= 0 && ct && (ct[0] & 0xffff) == OpTypeFloat && ct[2] == 32 && nc >= 1 && nc <= 4) { tv[x[2]] = (uint8_t)(1 + s); ncomp[x[2]] = nc; } }
        i += n; }
    // chains into the outputs, and outputs read back (not rewritten)
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 5 && x[3] < bound && tv[x[3]] && x[2] < bound) { tv[x[2]] = (uint8_t)(tv[x[3]] + 2); ncomp[x[2]] = 1; }   // 3 / 4: a scalar component
        if (op == 61 /* OpLoad */ && n >= 4 && x[3] < bound && tv[x[3]]) { free(loc); free(tv); free(ncomp); return NULL; }
        i += n; }
    int any = 0; for (uint32_t i = 0; i < bound; i++) if (tv[i] == 1 || tv[i] == 2) any = 1;
    if (!any) { free(loc); free(tv); free(ncomp); return NULL; }
    size_t cap = words * 2 + 256; uint32_t *o = malloc(4 * cap), nb = bound; size_t w = 5; memcpy(o, code, 20);
    int emitted = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (w + n + 64 > cap) { cap = cap * 2 + n; o = realloc(o, 4 * cap); }
        if (op == 54 /* OpFunction */ && !emitted) {   // before the first function: the integer types still missing, then the rewritten output variables
            emitted = 1;
            for (int s = 0; s < 2; s++) {
                int need = 0; for (uint32_t v = 0; v < bound; v++) if ((tv[v] == 1 + s) || (tv[v] == 3 + s)) need = 1;
                if (!need) continue;
                if (!id_int[s]) { id_int[s] = nb++; o[w++] = 4u << 16 | OpTypeInt; o[w++] = id_int[s]; o[w++] = 32; o[w++] = (uint32_t)s; }
                if (!id_ps[s]) { id_ps[s] = nb++; o[w++] = 4u << 16 | OpTypePointer; o[w++] = id_ps[s]; o[w++] = SC_Output; o[w++] = id_int[s]; }
                for (uint32_t v = 0; v < bound; v++) if (tv[v] == 1 + s && ncomp[v] >= 2) { uint32_t c = ncomp[v];
                    if (!id_vec[s][c]) { id_vec[s][c] = nb++; o[w++] = 4u << 16 | OpTypeVector; o[w++] = id_vec[s][c]; o[w++] = id_int[s]; o[w++] = c; }
                    if (!id_pv[s][c]) { id_pv[s][c] = nb++; o[w++] = 4u << 16 | OpTypePointer; o[w++] = id_pv[s][c]; o[w++] = SC_Output; o[w++] = id_vec[s][c]; } }
            }
            for (uint32_t v = 0; v < bound; v++) if (tv[v] == 1 || tv[v] == 2) { int s = tv[v] - 1; uint32_t c = ncomp[v];
                o[w++] = 4u << 16 | OpVariable; o[w++] = c >= 2 ? id_pv[s][c] : id_ps[s]; o[w++] = v; o[w++] = SC_Output; }
        }
        if (op == OpVariable && n >= 4 && x[2] < bound && (tv[x[2]] == 1 || tv[x[2]] == 2)) { i += n; continue; }   // moved below the new types
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 5 && x[2] < bound && tv[x[2]] >= 3) {
            memcpy(o + w, x, 4 * n); o[w + 1] = id_ps[tv[x[2]] - 3]; w += n; i += n; continue; }
        if (op == 62 /* OpStore */ && n >= 3 && x[1] < bound && tv[x[1]]) {
            int s = (tv[x[1]] - 1) & 1; uint32_t c = ncomp[x[1]]; uint32_t ty = c >= 2 && tv[x[1]] <= 2 ? id_vec[s][c] : id_int[s], b = nb++;
            o[w++] = 4u << 16 | 124 /* OpBitcast */; o[w++] = ty; o[w++] = b; o[w++] = x[2];
            o[w++] = (uint32_t)n << 16 | 62; o[w++] = x[1]; o[w++] = b; for (uint32_t k = 3; k < n; k++) o[w++] = x[k];
            i += n; continue; }
        memcpy(o + w, x, 4 * n); w += n; i += n;
    }
    o[3] = nb; *out_words = w; free(loc); free(tv); free(ncomp);
    return emitted ? o : (free(o), NULL);
}

// Transform feedback capture (nvn_vk.c tf_draw; the Metal side's main0_tf from dmsh_build.py vs_tf): a copy of a vertex shader that, before each return of its entry function, stores
// the captured outputs as their bits into storage buffers: buffer b (binding binding0 + b, set 0) gets output tf[b][0]'s first tf[b][1] components at word
// (VertexIndex - pc.first) * tf[b][1] + pc.base[b] + i, with pc = push constants { uint first; uint base[4]; }. NULL if the code cannot be read
uint32_t *spv_tf_variant(const uint32_t *code, size_t words, const int tf[4][2], uint32_t binding0, size_t *out_words) {
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    int32_t *loc = malloc(sizeof(int32_t) * bound); uint8_t *bi = calloc(bound, 1); for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    uint32_t entry = 0, t_uint = 0, t_int = 0, vid = 0, vid_ty = 0, outv[64] = {0}, out_ty[64] = {0}, out_nc[64] = {0}, out_ct[64] = {0};   // out_ct: component class 0 float / 1 uint / 2 sint
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) { free(loc); free(bi); return NULL; } const uint32_t *x = code + i;
        if (op == OpDecorate && n >= 4 && x[1] < bound) { if (x[2] == DecLocation) loc[x[1]] = (int32_t)x[3]; else if (x[2] == DecBuiltIn) { bi[x[1]] = 1; if (x[3] == 42 /* VertexIndex */) vid = x[1]; } }
        else if (op == OpEntryPoint && n >= 3 && !entry) entry = x[2];
        else if (op == OpTypeInt && n >= 4 && x[2] == 32) { if (x[3]) { if (!t_int) t_int = x[1]; } else if (!t_uint) t_uint = x[1]; }
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[2] < bound) {
            const uint32_t *p = def_of(code, words, x[1]); uint32_t pointee = p && (p[0] & 0xffff) == OpTypePointer ? p[3] : 0;
            if (x[2] == vid) vid_ty = pointee;
            if (x[3] == SC_Output && loc[x[2]] >= 0 && loc[x[2]] < 64 && !bi[x[2]] && pointee) {
                const uint32_t *t = def_of(code, words, pointee), *c = t; uint32_t nc = 1;
                if (t && (t[0] & 0xffff) == OpTypeVector) { nc = t[3]; c = def_of(code, words, t[2]); }
                if (c && ((c[0] & 0xffff) == OpTypeFloat || (c[0] & 0xffff) == OpTypeInt) && c[2] == 32) {
                    int l = loc[x[2]]; outv[l] = x[2]; out_ty[l] = pointee; out_nc[l] = nc; out_ct[l] = (c[0] & 0xffff) == OpTypeFloat ? 0 : c[3] ? 2 : 1;
                    if (nc > 1) out_ct[l] |= (uint32_t)t[2] << 2; else out_ct[l] |= (uint32_t)pointee << 2; } }   // the component type id, above the class bits
        }
        i += n; }
    free(loc); free(bi);
    if (!entry) return NULL;
    int pre13 = code[1] < 0x00010300u, pre14 = code[1] < 0x00010400u; uint32_t sc = pre13 ? 2u /* Uniform */ : (uint32_t)SC_StorageBuffer;
    uint32_t nb = bound;
    uint32_t n_uint = t_uint ? 0 : nb++, n_int = (vid || t_int) ? 0 : nb++; if (!t_uint) t_uint = n_uint; if (!t_int && !vid) t_int = n_int;
    uint32_t c[6]; for (int k = 0; k < 6; k++) c[k] = nb++;
    uint32_t t_rarr = nb++, t_sbs = nb++, t_psbs = nb++, t_pu = nb++, t_pcs = nb++, t_ppcs = nb++, t_ppu = nb++, v_pc = nb++, v_sb[4] = {0};
    for (int b = 0; b < 4; b++) if (tf[b][1]) v_sb[b] = nb++;
    uint32_t t_pint = 0; if (!vid) { t_pint = nb++; vid = nb++; vid_ty = t_int; }
    size_t cap = words * 2 + 1024; uint32_t *o = malloc(4 * cap); size_t w = 5; memcpy(o, code, 20);
    int decorated = 0, typed = 0, in_entry = 0;
    #define E(...) do { uint32_t e_[] = { __VA_ARGS__ }; size_t k_ = sizeof e_ / 4; if (w + k_ + 8 > cap) { cap = cap * 2 + k_; o = realloc(o, 4 * cap); } memcpy(o + w, e_, 4 * k_); w += k_; } while (0)
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (w + n + 256 > cap) { cap = cap * 2 + n; o = realloc(o, 4 * cap); }
        if (op == OpEntryPoint && x[2] == entry) {   // the interface: the vertex index (Input), and before SPIR-V 1.4 only Input / Output variables
            size_t at = w; memcpy(o + w, x, 4 * n); w += n;
            if (t_pint) o[w++] = vid;
            if (!pre14) { o[w++] = v_pc; for (int b = 0; b < 4; b++) if (v_sb[b]) o[w++] = v_sb[b]; }
            o[at] = (uint32_t)(w - at) << 16 | op; i += n; continue; }
        if (!decorated && op >= 19 && op <= 39) {   // the first type: annotations end here
            decorated = 1;
            E(4u << 16 | OpDecorate, t_rarr, 6 /* ArrayStride */, 4); E(3u << 16 | OpDecorate, t_sbs, pre13 ? (uint32_t)DecBufferBlock : (uint32_t)DecBlock); E(5u << 16 | 72, t_sbs, 0, 35 /* Offset */, 0);
            E(3u << 16 | OpDecorate, t_pcs, DecBlock); for (uint32_t m = 0; m < 5; m++) E(5u << 16 | 72, t_pcs, m, 35, 4 * m);
            for (int b = 0; b < 4; b++) if (v_sb[b]) { E(4u << 16 | OpDecorate, v_sb[b], DecDescriptorSet, 0); E(4u << 16 | OpDecorate, v_sb[b], DecBinding, binding0 + (uint32_t)b); }
            if (t_pint) E(4u << 16 | OpDecorate, vid, DecBuiltIn, 42);
        }
        if (!typed && op == 54 /* OpFunction */) {   // before the first function: the new types, constants and variables
            typed = 1;
            if (n_uint) E(4u << 16 | OpTypeInt, n_uint, 32, 0);
            if (n_int) E(4u << 16 | OpTypeInt, n_int, 32, 1);
            for (uint32_t k = 0; k < 6; k++) E(4u << 16 | OpConstant, t_uint, c[k], k);
            E(3u << 16 | OpTypeRuntimeArray, t_rarr, t_uint); E(3u << 16 | OpTypeStruct, t_sbs, t_rarr);
            E(4u << 16 | OpTypePointer, t_psbs, sc, t_sbs); E(4u << 16 | OpTypePointer, t_pu, sc, t_uint);
            E(7u << 16 | OpTypeStruct, t_pcs, t_uint, t_uint, t_uint, t_uint, t_uint);
            E(4u << 16 | OpTypePointer, t_ppcs, 9 /* PushConstant */, t_pcs); E(4u << 16 | OpTypePointer, t_ppu, 9, t_uint);
            E(4u << 16 | OpVariable, t_ppcs, v_pc, 9);
            for (int b = 0; b < 4; b++) if (v_sb[b]) E(4u << 16 | OpVariable, t_psbs, v_sb[b], sc);
            if (t_pint) { E(4u << 16 | OpTypePointer, t_pint, SC_Input, t_int); E(4u << 16 | OpVariable, t_pint, vid, SC_Input); }
        }
        if (op == 54 && n >= 3) in_entry = x[2] == entry;
        if (in_entry && op == 253 /* OpReturn */) {
            uint32_t vi = nb++, viu = vi, pf = nb++, first = nb++, rel = nb++;
            E(4u << 16 | 61 /* OpLoad */, vid_ty, vi, vid);
            if (vid_ty != t_uint) { viu = nb++; E(4u << 16 | 124 /* OpBitcast */, t_uint, viu, vi); }
            E(5u << 16 | 65, t_ppu, pf, v_pc, c[0]); E(4u << 16 | 61, t_uint, first, pf); E(5u << 16 | 130 /* OpISub */, t_uint, rel, viu, first);
            for (int b = 0; b < 4; b++) { if (!v_sb[b]) continue; int l = tf[b][0], nc = tf[b][1];
                uint32_t pb = nb++, base = nb++, m = nb++, row = nb++, val = 0;
                E(5u << 16 | 65, t_ppu, pb, v_pc, c[1 + b]); E(4u << 16 | 61, t_uint, base, pb);
                E(5u << 16 | 132 /* OpIMul */, t_uint, m, rel, c[nc]); E(5u << 16 | 128 /* OpIAdd */, t_uint, row, m, base);
                if (l >= 0 && l < 64 && outv[l]) { val = nb++; E(4u << 16 | 61, out_ty[l], val, outv[l]); }
                for (int k = 0; k < nc; k++) {
                    uint32_t bits = c[0];
                    if (val && (uint32_t)k < out_nc[l]) {
                        uint32_t comp = val; if (out_nc[l] > 1) { comp = nb++; E(5u << 16 | 81 /* OpCompositeExtract */, out_ct[l] >> 2, comp, val, (uint32_t)k); }
                        bits = comp; if ((out_ct[l] & 3) != 1) { bits = nb++; E(4u << 16 | 124, t_uint, bits, comp); } }
                    uint32_t idx = nb++, p = nb++; E(5u << 16 | 128, t_uint, idx, row, c[k]); E(6u << 16 | 65, t_pu, p, v_sb[b], c[0], idx); E(3u << 16 | 62 /* OpStore */, p, bits);
                }
            }
        }
        memcpy(o + w, x, 4 * n); w += n; i += n;
    }
    #undef E
    if (!typed || !decorated) { free(o); return NULL; }
    o[3] = nb; *out_words = w;
    return o;
}

// Transform feedback capture as points, for drivers whose vertex shaders cannot store into buffers (Arm's own Mali driver: vertexPipelineStoresAndAtomics off, the
//   self-test's A and B write nothing while its compute leg C writes all: the player, Pokemon and NPCs vanish, Arceus #47 #51 #54 #57 #58). A copy of a vertex shader
//   that, before each return of its entry function, puts the vertex on a pixel of its own in a W x H target: column (i % W), row (i / W) for i = VertexIndex - pc.first,
//   H = pc.h, gl_PointSize 1; and writes the captured outputs as their bits into new outputs: buffer b's output tf[b][0], its first tf[b][1] components, at Location 28 + b
//   as a uvec4 (the rest 0). Drawn as points with tf_points.frag, which writes each into an RGBA32UI colour target, then copied into the capture buffers by
//   tf_repack.comp (compute writes work there). Push constants { uint first; uint h; uint pad[3]; } (the layout spv_tf_variant's have). NULL if the code cannot be read
uint32_t *spv_tf_points_variant(const uint32_t *code, size_t words, const int tf[4][2], uint32_t w_px, size_t *out_words) {
    enum { OpMemberDecorate = 72, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpCompositeConstruct = 80, OpCompositeExtract = 81, OpConvertUToF = 112, OpBitcast = 124,
           OpISub = 130, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpUDiv = 134, OpFDiv = 136, OpUMod = 137, SC_Output = 3, SC_PushConstant = 9 };
    if (words < 5 || code[0] != 0x07230203u || !w_px) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    int32_t *loc = malloc(sizeof(int32_t) * bound); uint8_t *bi = calloc(bound, 1); if (!loc || !bi) { free(loc); free(bi); return NULL; }
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    uint32_t entry = 0, t_uint = 0, t_int = 0, t_float = 0, vid = 0, vid_ty = 0, outv[64] = {0}, out_ty[64] = {0}, out_nc[64] = {0}, out_ct[64] = {0};
    uint32_t blk = 0, blk_ps = 0, pos_var = 0, t_v4f = 0, t_v4u = 0, t_pov4f = 0, t_pof = 0, t_pov4u = 0, blk_var = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) { free(loc); free(bi); return NULL; } const uint32_t *x = code + i;
        if (op == OpDecorate && n >= 4 && x[1] < bound) { if (x[2] == DecLocation) loc[x[1]] = (int32_t)x[3]; else if (x[2] == DecBuiltIn) { bi[x[1]] = 1; if (x[3] == 42 /* VertexIndex */) vid = x[1]; if (x[3] == 0 /* Position */) pos_var = x[1]; } }
        else if (op == OpMemberDecorate && n >= 5 && x[3] == DecBuiltIn) { if (x[2] == 0 && x[4] == 0) blk = x[1]; if (x[2] == 1 && x[4] == 1) blk_ps = x[1]; }
        else if (op == OpEntryPoint && n >= 3 && !entry) entry = x[2];
        else if (op == OpTypeInt && n >= 4 && x[2] == 32) { if (x[3]) { if (!t_int) t_int = x[1]; } else if (!t_uint) t_uint = x[1]; }
        else if (op == OpTypeFloat && n >= 3 && x[2] == 32 && !t_float) t_float = x[1];
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpTypeVector && n >= 4 && x[3] == 4) { if (t_float && x[2] == t_float && !t_v4f) t_v4f = x[1]; if (t_uint && x[2] == t_uint && !t_v4u) t_v4u = x[1]; }
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpTypePointer && n >= 4 && x[2] == SC_Output) { if (t_v4f && x[3] == t_v4f && !t_pov4f) t_pov4f = x[1]; if (t_float && x[3] == t_float && !t_pof) t_pof = x[1]; if (t_v4u && x[3] == t_v4u && !t_pov4u) t_pov4u = x[1]; }
        if (op == OpVariable && n >= 4 && x[2] < bound) {
            const uint32_t *p = def_of(code, words, x[1]); uint32_t pointee = p && (p[0] & 0xffff) == OpTypePointer ? p[3] : 0;
            if (x[2] == vid) vid_ty = pointee;
            if (blk && pointee == blk && x[3] == SC_Output) blk_var = x[2];
            if (x[3] == SC_Output && loc[x[2]] >= 0 && loc[x[2]] < 64 && !bi[x[2]] && pointee) {
                const uint32_t *t = def_of(code, words, pointee), *c = t; uint32_t nc = 1;
                if (t && (t[0] & 0xffff) == OpTypeVector) { nc = t[3]; c = def_of(code, words, t[2]); }
                if (c && ((c[0] & 0xffff) == OpTypeFloat || (c[0] & 0xffff) == OpTypeInt) && c[2] == 32) {
                    int l = loc[x[2]]; outv[l] = x[2]; out_ty[l] = pointee; out_nc[l] = nc; out_ct[l] = (c[0] & 0xffff) == OpTypeFloat ? 0 : c[3] ? 2 : 1;
                    if (nc > 1) out_ct[l] |= (uint32_t)t[2] << 2; else out_ct[l] |= (uint32_t)pointee << 2; } }
        }
        i += n; }
    int used28 = 0; for (uint32_t v = 0; v < bound; v++) if (loc[v] >= 28 && loc[v] < 32) used28 = 1;
    free(loc); free(bi);
    if (!entry || !t_float || !t_v4f || (!blk_var && !pos_var) || used28) return NULL;   // (no float vec4, no position to place, or Location 28-31 already in use)
    int pre14 = code[1] < 0x00010400u;
    uint32_t nb = bound;
    uint32_t n_uint = t_uint ? 0 : nb++; if (!t_uint) t_uint = n_uint;
    uint32_t n_v4u = t_v4u ? 0 : nb++; if (!t_v4u) t_v4u = n_v4u;
    uint32_t n_pov4u = t_pov4u ? 0 : nb++; if (!t_pov4u) t_pov4u = n_pov4u;
    uint32_t n_pov4f = t_pov4f ? 0 : nb++; if (!t_pov4f) t_pov4f = n_pov4f;
    uint32_t n_pof = t_pof ? 0 : nb++; if (!t_pof) t_pof = n_pof;
    uint32_t c[6]; for (int k = 0; k < 6; k++) c[k] = nb++;
    uint32_t c_w = nb++, f_half = nb++, f_one = nb++, f_two = nb++, f_zero = nb++, f_scale = nb++;
    uint32_t t_pcs = nb++, t_ppcs = nb++, t_ppu = nb++, v_pc = nb++, v_out[4] = {0};
    for (int b = 0; b < 4; b++) if (tf[b][1]) v_out[b] = nb++;
    uint32_t t_pint = 0; if (!vid) { if (!t_int) return NULL; t_pint = nb++; vid = nb++; vid_ty = t_int; }
    size_t cap = words * 2 + 2048; uint32_t *o = malloc(4 * cap); if (!o) return NULL; size_t w = 5; memcpy(o, code, 20);
    int decorated = 0, typed = 0, in_entry = 0;
    #define E(...) do { uint32_t e_[] = { __VA_ARGS__ }; size_t k_ = sizeof e_ / 4; if (w + k_ + 8 > cap) { cap = cap * 2 + k_; o = realloc(o, 4 * cap); } memcpy(o + w, e_, 4 * k_); w += k_; } while (0)
    union { float f; uint32_t u; } fv;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (w + n + 512 > cap) { cap = cap * 2 + n; o = realloc(o, 4 * cap); }
        if (op == OpEntryPoint && x[2] == entry) {   // the interface: the new outputs, the vertex index if it is new, and from SPIR-V 1.4 the push constants
            size_t at = w; memcpy(o + w, x, 4 * n); w += n;
            if (t_pint) o[w++] = vid;
            for (int b = 0; b < 4; b++) if (v_out[b]) o[w++] = v_out[b];
            if (!pre14) o[w++] = v_pc;
            o[at] = (uint32_t)(w - at) << 16 | op; i += n; continue; }
        if (!decorated && op >= 19 && op <= 39) {   // the first type: annotations end here
            decorated = 1;
            for (int b = 0; b < 4; b++) if (v_out[b]) E(4u << 16 | OpDecorate, v_out[b], DecLocation, 28u + (uint32_t)b);
            E(3u << 16 | OpDecorate, t_pcs, DecBlock); for (uint32_t m = 0; m < 5; m++) E(5u << 16 | OpMemberDecorate, t_pcs, m, 35 /* Offset */, 4 * m);
            if (t_pint) E(4u << 16 | OpDecorate, vid, DecBuiltIn, 42);
        }
        if (!typed && op == 54 /* OpFunction */) {   // before the first function: the new types, constants and variables
            typed = 1;
            if (n_uint) E(4u << 16 | OpTypeInt, n_uint, 32, 0);
            if (n_v4u) E(4u << 16 | OpTypeVector, n_v4u, t_uint, 4);
            if (n_pov4u) E(4u << 16 | OpTypePointer, n_pov4u, SC_Output, t_v4u);
            if (n_pov4f) E(4u << 16 | OpTypePointer, n_pov4f, SC_Output, t_v4f);
            if (n_pof) E(4u << 16 | OpTypePointer, n_pof, SC_Output, t_float);
            for (uint32_t k = 0; k < 6; k++) E(4u << 16 | OpConstant, t_uint, c[k], k);
            E(4u << 16 | OpConstant, t_uint, c_w, w_px);
            fv.f = 0.5f; E(4u << 16 | OpConstant, t_float, f_half, fv.u); fv.f = 1.0f; E(4u << 16 | OpConstant, t_float, f_one, fv.u);
            fv.f = 2.0f; E(4u << 16 | OpConstant, t_float, f_two, fv.u); fv.f = 0.0f; E(4u << 16 | OpConstant, t_float, f_zero, fv.u);
            fv.f = 2.0f / (float)w_px; E(4u << 16 | OpConstant, t_float, f_scale, fv.u);
            E(7u << 16 | OpTypeStruct, t_pcs, t_uint, t_uint, t_uint, t_uint, t_uint);
            E(4u << 16 | OpTypePointer, t_ppcs, SC_PushConstant, t_pcs); E(4u << 16 | OpTypePointer, t_ppu, SC_PushConstant, t_uint);
            E(4u << 16 | OpVariable, t_ppcs, v_pc, SC_PushConstant);
            for (int b = 0; b < 4; b++) if (v_out[b]) E(4u << 16 | OpVariable, t_pov4u, v_out[b], SC_Output);
            if (t_pint) { E(4u << 16 | OpTypePointer, t_pint, SC_Input, t_int); E(4u << 16 | OpVariable, t_pint, vid, SC_Input); }
        }
        if (op == 54 && n >= 3) in_entry = x[2] == entry;
        if (in_entry && op == 253 /* OpReturn */) {
            uint32_t vi = nb++, viu = vi, pf = nb++, first = nb++, ph = nb++, hh = nb++, rel = nb++, col = nb++, row = nb++, fx = nb++, fy = nb++, fh = nb++;
            uint32_t x1 = nb++, x2 = nb++, px = nb++, y1 = nb++, y2 = nb++, y3 = nb++, py = nb++, pos = nb++;
            E(4u << 16 | OpLoad, vid_ty, vi, vid);
            if (vid_ty != t_uint) { viu = nb++; E(4u << 16 | OpBitcast, t_uint, viu, vi); }
            E(5u << 16 | OpAccessChain, t_ppu, pf, v_pc, c[0]); E(4u << 16 | OpLoad, t_uint, first, pf);
            E(5u << 16 | OpAccessChain, t_ppu, ph, v_pc, c[1]); E(4u << 16 | OpLoad, t_uint, hh, ph);
            E(5u << 16 | OpISub, t_uint, rel, viu, first); E(5u << 16 | OpUMod, t_uint, col, rel, c_w); E(5u << 16 | OpUDiv, t_uint, row, rel, c_w);
            E(4u << 16 | OpConvertUToF, t_float, fx, col); E(4u << 16 | OpConvertUToF, t_float, fy, row); E(4u << 16 | OpConvertUToF, t_float, fh, hh);
            E(5u << 16 | OpFAdd, t_float, x1, fx, f_half); E(5u << 16 | OpFMul, t_float, x2, x1, f_scale); E(5u << 16 | OpFSub, t_float, px, x2, f_one);
            E(5u << 16 | OpFAdd, t_float, y1, fy, f_half); E(5u << 16 | OpFMul, t_float, y2, y1, f_two); E(5u << 16 | OpFDiv, t_float, y3, y2, fh); E(5u << 16 | OpFSub, t_float, py, y3, f_one);
            E(7u << 16 | OpCompositeConstruct, t_v4f, pos, px, py, f_zero, f_one);
            if (blk_var) { uint32_t pp = nb++; E(5u << 16 | OpAccessChain, t_pov4f, pp, blk_var, c[0]); E(3u << 16 | OpStore, pp, pos);
                           if (blk_ps == blk) { uint32_t ps = nb++; E(5u << 16 | OpAccessChain, t_pof, ps, blk_var, c[1]); E(3u << 16 | OpStore, ps, f_one); } }
            else E(3u << 16 | OpStore, pos_var, pos);
            for (int b = 0; b < 4; b++) { if (!v_out[b]) continue; int l = tf[b][0], nc = tf[b][1]; uint32_t val = 0, comps[4];
                if (l >= 0 && l < 64 && outv[l]) { val = nb++; E(4u << 16 | OpLoad, out_ty[l], val, outv[l]); }
                for (int k = 0; k < 4; k++) {
                    uint32_t bits = c[0];
                    if (k < nc && val && (uint32_t)k < out_nc[l]) {
                        uint32_t comp = val; if (out_nc[l] > 1) { comp = nb++; E(5u << 16 | OpCompositeExtract, out_ct[l] >> 2, comp, val, (uint32_t)k); }
                        bits = comp; if ((out_ct[l] & 3) != 1) { bits = nb++; E(4u << 16 | OpBitcast, t_uint, bits, comp); } }
                    comps[k] = bits; }
                uint32_t v = nb++; E(7u << 16 | OpCompositeConstruct, t_v4u, v, comps[0], comps[1], comps[2], comps[3]); E(3u << 16 | OpStore, v_out[b], v);
            }
        }
        memcpy(o + w, x, 4 * n); w += n; i += n;
    }
    #undef E
    if (!typed || !decorated) { free(o); return NULL; }
    o[3] = nb; *out_words = w;
    return o;
}

// Storage images without a format that the shader writes / reads need StorageImageWriteWithoutFormat / StorageImageReadWithoutFormat, which the translator does not always
// declare (Pokémon Legends: Arceus compute shaders: spirv-val error on OpImageWrite; a strict driver may refuse the module). A copy with the missing ones added; NULL if none is missing
uint32_t *spv_image_caps(const uint32_t *code, size_t words, size_t *out_words) {
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    int has_w = 0, has_r = 0, unfmt = 0, wr = 0, rd = 0; size_t last_cap = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return NULL; const uint32_t *x = code + i;
        if (op == 17 /* OpCapability */ && n == 2) { if (x[1] == 56) has_w = 1; if (x[1] == 55) has_r = 1; last_cap = i + n; }
        else if (op == OpTypeImage && n >= 9 && x[7] == 2 && x[8] == 0) unfmt = 1;   // a storage image with format Unknown
        else if (op == 99 /* OpImageWrite */) wr = 1;
        else if (op == 98 /* OpImageRead */) rd = 1;
        i += n; }
    int add_w = unfmt && wr && !has_w, add_r = unfmt && rd && !has_r;
    if (!(add_w || add_r) || !last_cap) return NULL;
    uint32_t *o = malloc(4 * (words + 4)); size_t w = 0;
    memcpy(o, code, 4 * last_cap); w = last_cap;
    if (add_w) { o[w++] = 2u << 16 | 17; o[w++] = 56; }
    if (add_r) { o[w++] = 2u << 16 | 17; o[w++] = 55; }
    memcpy(o + w, code + last_cap, 4 * (words - last_cap)); w += words - last_cap;
    *out_words = w; return o;
}
// Storage images declared in one format re-declared in another, in place (the operand only): an R11G11B10F image the engine stands in for by RGBA16F
//   (DM_VK_B10_AS_RGBA16) is given to the shader as RGBA16F, which its declaration must then name. Rgba16f needs no capability beyond Shader. Storage texel
//   buffers (Dim Buffer) keep their format: a buffer texture stays in the packed format (its texels are the game's memory). Returns how many changed
int spv_image_format_swap(uint32_t *code, size_t words, uint32_t from, uint32_t to) {
    if (words < 5 || code[0] != 0x07230203u) return 0;
    int n = 0;
    for (size_t i = 5; i < words;) { uint32_t wc = code[i] >> 16, op = code[i] & 0xffff; if (!wc || i + wc > words) return n;
        if (op == OpTypeImage && wc >= 9 && code[i + 8] == from && code[i + 3] != SPV_DIM_BUFFER) { code[i + 8] = to; n++; }
        i += wc; }
    return n;
}
// Gathers nudged by 1/512 of a texel (Adreno). On a coordinate right at a texel centre the host picks another 2x2 block than Maxwell does; Ryujinx's translator adds
//   1 / (size * 2^(8+1)) to the coordinate for such hosts (TexturePass, QueryHostGatherBiasPrecision 8), and Lemon found Adreno is one (64x64 dark shadow squares in
//   Tears of the Kingdom). Done here on the loaded SPIR-V, so shaders translated earlier get it too. 2D and 2D-array gathers only (x and y; a layer stays as it is)
uint32_t *spv_gather_bias(const uint32_t *code, size_t words, size_t *out_words) {
    enum { OpCapability = 17, OpFunction = 54, OpConstantComposite = 44, OpVectorShuffle = 79, OpImageGather = 96, OpImageDrefGather = 97,
           OpImage = 100, OpImageQuerySizeLod = 103, OpConvertSToF = 111, OpConvertUToF = 112, OpFAdd = 129, OpFDiv = 136, CapImageQuery = 50 };
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint32_t *ty = calloc(bound, 4), *info = calloc(bound, 4);   // ty: an id's result type; info: per type id, what it is (below)
    if (!ty || !info) { free(ty); free(info); return NULL; }
    // info: 1 = f32, 2 = i32 signed, 3 = i32 unsigned; vectors 0x100 | count << 4 | component kind; image 0x1000 | dim << 4 | arrayed << 2 | ms << 1 | sampled==1; sampled image 0x2000 (its image in ty[])
    size_t first_fn = 0, last_cap = 0; int has_cap = 0, ngath = 0; uint32_t f32 = 0, i32 = 0, i32u = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) { free(ty); free(info); return NULL; } const uint32_t *x = code + i;
        if (op == OpCapability && n == 2) { last_cap = i + n; if (x[1] == CapImageQuery) has_cap = 1; }
        else if (op == OpTypeFloat && n >= 3 && x[2] == 32 && x[1] < bound) { info[x[1]] = 1; if (!f32) f32 = x[1]; }
        else if (op == OpTypeInt && n == 4 && x[2] == 32 && x[1] < bound) { info[x[1]] = x[3] ? 2 : 3; if (x[3] && !i32) i32 = x[1]; if (!x[3] && !i32u) i32u = x[1]; }
        else if (op == OpTypeVector && n == 4 && x[1] < bound && x[2] < bound && info[x[2]] && info[x[2]] < 4) info[x[1]] = 0x100 | x[3] << 4 | info[x[2]];
        else if (op == OpTypeImage && n >= 9 && x[1] < bound) info[x[1]] = 0x1000 | (x[3] & 0xf) << 4 | (x[5] ? 4 : 0) | (x[6] ? 2 : 0) | (x[7] == 1);
        else if (op == OpTypeSampledImage && n == 3 && x[1] < bound && x[2] < bound) { info[x[1]] = 0x2000; ty[x[1]] = x[2]; }
        else if (op == OpFunction && !first_fn) first_fn = i;
        else if ((op == OpImageGather || op == OpImageDrefGather) && n >= 5) ngath++;
        // result-bearing instructions inside functions with a result type at x[1] and id at x[2] (what a gather's operands come from)
        if (first_fn && n >= 3 && (op == 61 /* OpLoad */ || op == 86 /* OpSampledImage */ || op == 80 /* OpCompositeConstruct */ || op == OpVectorShuffle || op == 81 /* OpCompositeExtract */
            || (op >= 109 && op <= 200) /* conversions, arithmetic, logic */ || op == 245 /* OpPhi */) && x[2] < bound) ty[x[2]] = x[1];
        if (op == 43 /* OpConstant */ && n >= 4 && x[2] < bound) ty[x[2]] = x[1];
        if (op == OpConstantComposite && n >= 3 && x[2] < bound) ty[x[2]] = x[1];
        if (op == 59 /* OpVariable */ && n >= 4 && x[2] < bound) ty[x[2]] = x[1];
        i += n; }
    if (!ngath || !first_fn || !f32 || !last_cap) { free(ty); free(info); return NULL; }
    uint32_t it = i32 ? i32 : i32u ? i32u : 0, ik = i32 ? 2 : i32u ? 3 : 2, next = bound;
    uint32_t *o = malloc(4 * (words + 64 + 40 * (size_t)ngath)); if (!o) { free(ty); free(info); return NULL; }
    size_t w = 0; memcpy(o, code, 4 * last_cap); w = last_cap;
    if (!has_cap) { o[w++] = 2u << 16 | OpCapability; o[w++] = CapImageQuery; }
    memcpy(o + w, code + last_cap, 4 * (first_fn - last_cap)); w += first_fn - last_cap;
    // the types and constants the nudge needs, declared once before the first function where the shader lacks them
    uint32_t vec[4][4] = {{0}};   // vec[kind][count]
    for (uint32_t id = 1; id < bound; id++) if ((info[id] & 0xf00) == 0x100) { uint32_t c = info[id] >> 4 & 0xf, k = info[id] & 0xf; if (c < 4 && (k == 1 || k == ik) && !vec[k][c]) vec[k][c] = id; }
    if (!it) { it = next++; o[w++] = 4u << 16 | OpTypeInt; o[w++] = it; o[w++] = 32; o[w++] = 1; }
#define NEED_VEC(k, c, comp) if (!vec[k][c]) { vec[k][c] = next++; o[w++] = 4u << 16 | OpTypeVector; o[w++] = vec[k][c]; o[w++] = comp; o[w++] = c; }
    NEED_VEC(1, 2, f32) NEED_VEC(1, 3, f32) NEED_VEC(ik, 2, it) NEED_VEC(ik, 3, it)
#undef NEED_VEC
    uint32_t c0 = next++, cn = next++, cn2 = next++;
    o[w++] = 4u << 16 | 43; o[w++] = it; o[w++] = c0; o[w++] = 0;
    o[w++] = 4u << 16 | 43; o[w++] = f32; o[w++] = cn; o[w++] = 0x3B000000u;   // 2^-9
    o[w++] = 5u << 16 | OpConstantComposite; o[w++] = vec[1][2]; o[w++] = cn2; o[w++] = cn; o[w++] = cn;
    int done = 0;
    for (size_t i = first_fn; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if ((op == OpImageGather || op == OpImageDrefGather) && n >= 5 && x[3] < bound && x[4] < bound) {
            uint32_t sit = ty[x[3]], imt = sit && sit < bound && info[sit] == 0x2000 ? ty[sit] : 0, ii = imt && imt < bound ? info[imt] : 0, ct = ty[x[4]];
            int dim2d = (ii & 0xf000) == 0x1000 && (ii >> 4 & 0xf) == 1 && !(ii & 2), arr = ii & 4;
            int cok = ct && ct < bound && info[ct] == (0x100u | (arr ? 3u : 2u) << 4 | 1u);
            if (dim2d && cok) {
                uint32_t img = next++, sz = next++, szf = next++, off = next++, nc = next++, xy = 0, sxy = 0, out = 0;
                o[w++] = 4u << 16 | OpImage; o[w++] = imt; o[w++] = img; o[w++] = x[3];
                o[w++] = 5u << 16 | OpImageQuerySizeLod; o[w++] = vec[ik][arr ? 3 : 2]; o[w++] = sz; o[w++] = img; o[w++] = c0;
                if (arr) { sxy = next++; o[w++] = 7u << 16 | OpVectorShuffle; o[w++] = vec[ik][2]; o[w++] = sxy; o[w++] = sz; o[w++] = sz; o[w++] = 0; o[w++] = 1;
                           xy = next++; o[w++] = 7u << 16 | OpVectorShuffle; o[w++] = vec[1][2]; o[w++] = xy; o[w++] = x[4]; o[w++] = x[4]; o[w++] = 0; o[w++] = 1; }
                o[w++] = 4u << 16 | (ik == 2 ? OpConvertSToF : OpConvertUToF); o[w++] = vec[1][2]; o[w++] = szf; o[w++] = arr ? sxy : sz;
                o[w++] = 5u << 16 | OpFDiv; o[w++] = vec[1][2]; o[w++] = off; o[w++] = cn2; o[w++] = szf;
                o[w++] = 5u << 16 | OpFAdd; o[w++] = vec[1][2]; o[w++] = nc; o[w++] = arr ? xy : x[4]; o[w++] = off;
                if (arr) { out = next++; o[w++] = 8u << 16 | OpVectorShuffle; o[w++] = vec[1][3]; o[w++] = out; o[w++] = nc; o[w++] = x[4]; o[w++] = 0; o[w++] = 1; o[w++] = 4; }
                memcpy(o + w, x, 4 * n); o[w + 4] = arr ? out : nc; w += n; i += n; done++; continue;
            }
        }
        memcpy(o + w, x, 4 * n); w += n; i += n; }
    free(ty); free(info);
    if (!done) { free(o); return NULL; }
    o[3] = next; *out_words = w; return o;
}

// textureQueryLod's first component (the mip level the sample would read) as clamp (computed LOD, 0, levels - 1), for Arm's own driver: on a Mali-G615 MC6 (stock
//   driver, Android 16) dEQP-VK.glsl.texture_functions.query.texturequerylod failed 63 of 190 cases, all the same way: the second component (the LOD) right, the first
//   one floor (LOD) whatever the texture's level count and the sampler's mip mode (level 2 on a texture with no mips; 2 where trilinear wants 2.43). Ryujinx's
//   translator writes Maxwell's TMML as OpImageQueryLod and reads component 0 only: 1,635 of Sword / Shield's 38,501 shaders. Not exact either: the sampler's min / max
//   LOD and a nearest mip mode are not known to the shader, so it is the answer for a linear-mip sampler without LOD clamps, the common case. 1D / 2D / 3D / cube
//   (arrayed or not) only, as OpImageQueryLevels allows. The query keeps its place under a new id; OpCompositeInsert takes the old id. NULL when the module has none
uint32_t *spv_query_lod(const uint32_t *code, size_t words, size_t *out_words) {
    enum { OpCapability = 17, OpFunction = 54, OpExtInstImport = 11, OpExtInst = 12, OpCompositeExtract = 81, OpCompositeInsert = 82, OpImage = 100,
           OpImageQueryLod = 105, OpImageQueryLevels = 106, OpConvertSToF = 111, OpConvertUToF = 112, OpFSub = 131, FClamp = 43, CapImageQuery = 50 };
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint32_t *ty = calloc(bound, 4), *info = calloc(bound, 4);   // ty: an id's result type (of a sampled image type: its image type); info: 1 f32, 2 / 3 i32 signed / unsigned, image 0x1000 | dim << 4 | arrayed << 2 | ms << 1 | sampled==1, sampled image 0x2000
    if (!ty || !info) { free(ty); free(info); return NULL; }
    size_t first_fn = 0, last_cap = 0; int has_cap = 0, nq = 0; uint32_t f32 = 0, i32 = 0, i32u = 0, glsl = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) { free(ty); free(info); return NULL; } const uint32_t *x = code + i;
        if (op == OpCapability && n == 2) { last_cap = i + n; if (x[1] == CapImageQuery) has_cap = 1; }
        else if (op == OpExtInstImport && n >= 3 && !strncmp((const char *)(x + 2), "GLSL.std.450", 4 * (n - 2))) glsl = x[1];
        else if (op == OpTypeFloat && n >= 3 && x[2] == 32 && x[1] < bound) { info[x[1]] = 1; if (!f32) f32 = x[1]; }
        else if (op == OpTypeInt && n == 4 && x[2] == 32 && x[1] < bound) { info[x[1]] = x[3] ? 2 : 3; if (x[3] && !i32) i32 = x[1]; if (!x[3] && !i32u) i32u = x[1]; }
        else if (op == OpTypeImage && n >= 9 && x[1] < bound) info[x[1]] = 0x1000 | (x[3] & 0xf) << 4 | (x[5] ? 4 : 0) | (x[6] ? 2 : 0) | (x[7] == 1);
        else if (op == OpTypeSampledImage && n == 3 && x[1] < bound && x[2] < bound) { info[x[1]] = 0x2000; ty[x[1]] = x[2]; }
        else if (op == OpFunction && !first_fn) first_fn = i;
        else if (op == OpImageQueryLod && n == 5) nq++;
        if (first_fn && n >= 3 && (op == 61 /* OpLoad */ || op == 86 /* OpSampledImage */ || op == 245 /* OpPhi */ || op == 169 /* OpSelect */) && x[2] < bound) ty[x[2]] = x[1];
        i += n; }
    if (!nq || !first_fn || !f32 || !glsl || !last_cap) { free(ty); free(info); return NULL; }
    uint32_t it = i32 ? i32 : i32u, next = bound;
    uint32_t *o = malloc(4 * (words + 32 + 32 * (size_t)nq)); if (!o) { free(ty); free(info); return NULL; }
    size_t w = 0; memcpy(o, code, 4 * last_cap); w = last_cap;
    if (!has_cap) { o[w++] = 2u << 16 | OpCapability; o[w++] = CapImageQuery; }
    memcpy(o + w, code + last_cap, 4 * (first_fn - last_cap)); w += first_fn - last_cap;
    if (!it) { it = next++; o[w++] = 4u << 16 | OpTypeInt; o[w++] = it; o[w++] = 32; o[w++] = 1; }
    int isigned = it != i32u;
    uint32_t c0 = next++, c1 = next++;
    o[w++] = 4u << 16 | 43 /* OpConstant */; o[w++] = f32; o[w++] = c0; o[w++] = 0;
    o[w++] = 4u << 16 | 43; o[w++] = f32; o[w++] = c1; o[w++] = 0x3F800000u;
    int done = 0;
    for (size_t i = first_fn; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpImageQueryLod && n == 5 && x[3] < bound) {
            uint32_t sit = ty[x[3]], imt = sit && sit < bound && info[sit] == 0x2000 ? ty[sit] : 0, ii = imt && imt < bound ? info[imt] : 0, dim = ii >> 4 & 0xf;
            if ((ii & 0xf000) == 0x1000 && dim <= 3 && !(ii & 2)) {   // Dim 1D / 2D / 3D / Cube, not multisampled
                uint32_t q = next++, img = next++, lv = next++, lvf = next++, mx = next++, y = next++, l = next++;
                memcpy(o + w, x, 4 * n); o[w + 2] = q; w += n;
                o[w++] = 4u << 16 | OpImage; o[w++] = imt; o[w++] = img; o[w++] = x[3];
                o[w++] = 4u << 16 | OpImageQueryLevels; o[w++] = it; o[w++] = lv; o[w++] = img;
                o[w++] = 4u << 16 | (isigned ? OpConvertSToF : OpConvertUToF); o[w++] = f32; o[w++] = lvf; o[w++] = lv;
                o[w++] = 5u << 16 | OpFSub; o[w++] = f32; o[w++] = mx; o[w++] = lvf; o[w++] = c1;
                o[w++] = 5u << 16 | OpCompositeExtract; o[w++] = f32; o[w++] = y; o[w++] = q; o[w++] = 1;
                o[w++] = 8u << 16 | OpExtInst; o[w++] = f32; o[w++] = l; o[w++] = glsl; o[w++] = FClamp; o[w++] = y; o[w++] = c0; o[w++] = mx;
                o[w++] = 6u << 16 | OpCompositeInsert; o[w++] = x[1]; o[w++] = x[2]; o[w++] = l; o[w++] = q; o[w++] = 0;
                i += n; done++; continue;
            }
        }
        memcpy(o + w, x, 4 * n); w += n; i += n; }
    free(ty); free(info);
    if (!done) { free(o); return NULL; }
    o[3] = next; *out_words = w; return o;
}

// Float controls (SPV_KHR_float_controls): a copy whose entry points ask for SignedZeroInfNanPreserve on 32-bit floats. Without it a driver may assume no value is ever
// infinite or NaN and optimize on that: ARM's Mali driver then drops the translated shaders' NaN guards (mix(x, 0, isnan(x)) before each texture read) and reorders
// maths that Maxwell evaluates exactly; the lit scene came out at the format's maximum on every Mali-G615 / G720 / G925 with ARM's driver, while PanVK on the same
// GPUs and every other driver drew it. NULL when the module already asks for it or is not one to patch
uint32_t *spv_float_controls(const uint32_t *code, size_t words, size_t *out_words) {
    enum { OpExtension = 10, OpCapability = 17, OpEntryPoint = 15, OpExecutionMode = 16, OpExecutionModeId = 331, OpMemoryModel = 14, OpExtInstImport = 11,
           CapSZINP = 4466, ModeSZINP = 4461 };
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    size_t last_cap = 0, last_ext = 0, last_mode = 0, nep = 0; int has_cap = 0, has_ext = 0;
    uint32_t ep[16];
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return NULL; const uint32_t *x = code + i;
        if (op == OpCapability && n == 2) { last_cap = i + n; if (x[1] == CapSZINP) has_cap = 1; }
        else if (op == OpExtension) { last_ext = i + n; if (n >= 2 && strstr((const char *)(x + 1), "float_controls")) has_ext = 1; }
        else if (op == OpEntryPoint && n >= 3) { if (nep < 16) ep[nep++] = x[2]; last_mode = i + n; }
        else if (op == OpExecutionMode || op == OpExecutionModeId) { if (n >= 4 && x[2] == ModeSZINP) return NULL; last_mode = i + n; }
        else if (op == 54 /* OpFunction */) break;
        i += n; }
    if (!nep || !last_cap || !last_mode) return NULL;
    static const char ext[] = "SPV_KHR_float_controls"; uint32_t extw = (uint32_t)((sizeof ext + 3) / 4);   // the string with its nul, in whole words
    int need_ext = !has_ext && code[1] < 0x10400;   // SPIR-V 1.4 has it in the core
    size_t add = (has_cap ? 0 : 2) + (need_ext ? 1 + extw : 0) + 4 * nep;
    uint32_t *o = malloc(4 * (words + add)); if (!o) return NULL;
    size_t w = 0, ins_ext = last_ext > last_cap ? last_ext : last_cap;
    for (size_t i = 0; i < words;) {
        size_t n = i < 5 ? 5 : code[i] >> 16; memcpy(o + w, code + i, 4 * n); w += n; i += n;
        if (i == last_cap && !has_cap) { o[w++] = 2u << 16 | OpCapability; o[w++] = CapSZINP; }
        if (i == ins_ext && need_ext) { o[w++] = (1 + extw) << 16 | OpExtension; memset(o + w, 0, 4 * extw); memcpy(o + w, ext, sizeof ext); w += extw; }
        if (i == last_mode) for (size_t k = 0; k < nep; k++) { o[w++] = 4u << 16 | OpExecutionMode; o[w++] = ep[k]; o[w++] = ModeSZINP; o[w++] = 32; }
    }
    *out_words = w; return o;
}

// Maxwell's min / max (FMNMX) give the operand that is not NaN, and its saturate (.SAT) gives 0 for NaN; the translator writes them as GLSL.std.450 FMin / FMax / FClamp,
// whose result for a NaN operand SPIR-V leaves undefined. Mesa's compilers (Turnip, PanVK) happen to give Maxwell's answer; a driver may instead pass the NaN on, and a NaN
// the game's shaders count on min / max / saturate to drop then reaches the picture. A copy with NMin / NMax / NClamp, which give Maxwell's answer by definition (the
// Mali white scene, #110: every lit surface at the format's maximum on Arm's own driver). Same length; NULL when the module has none
uint32_t *spv_nan_minmax(const uint32_t *code, size_t words, size_t *out_words) {
    enum { OpExtInstImport = 11, OpExtInst = 12, FMin = 37, FMax = 40, FClamp = 43, NMin = 79, NMax = 80, NClamp = 81 };
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t glsl = 0; size_t hits = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return NULL; const uint32_t *x = code + i;
        if (op == OpExtInstImport && n >= 3 && !strncmp((const char *)(x + 2), "GLSL.std.450", 4 * (n - 2))) glsl = x[1];
        else if (op == OpExtInst && n >= 5 && glsl && x[3] == glsl && (x[4] == FMin || x[4] == FMax || x[4] == FClamp)) hits++;
        i += n; }
    if (!hits) return NULL;
    uint32_t *o = malloc(4 * words); if (!o) return NULL; memcpy(o, code, 4 * words);
    for (size_t i = 5; i < words; i += o[i] >> 16) { uint32_t n = o[i] >> 16, *x = o + i;
        if ((x[0] & 0xffff) == OpExtInst && n >= 5 && x[3] == glsl) x[4] = x[4] == FMin ? NMin : x[4] == FMax ? NMax : x[4] == FClamp ? NClamp : x[4]; }
    *out_words = words; return o;
}

// The same answer written out, for a driver whose compiler does not honour the N forms: NMin / NMax / NClamp are in GLSL.std.450 from the start, but nothing checks that a
// driver gives them their NaN rule, and Arm's own driver kept drawing the scene at the format's maximum with spv_nan_minmax on (#111, #128: v0.59, r49p1). A copy in which
// every float min / max / clamp (F or N form, scalar or vector of 32-bit floats) is followed by the rule as OpIsNan + OpSelect, which float controls make the driver keep:
//   min / max (a, b): a is NaN -> b; b is NaN -> a; else the op's own result        clamp (x, lo, hi): x is NaN -> lo; else the op's own result
// The op keeps its place and operands under a new id (spv_clamp_zero's zero stays where that pass put it) and the last select takes the op's old id. bool / bvecN types
// are added before the first function where the module has none. NULL when the module has no such op
uint32_t *spv_nan_select(const uint32_t *code, size_t words, size_t *out_words) {
    enum { OpExtInstImport = 11, OpExtInst = 12, OpTypeBool = 20, OpTypeFloat = 22, OpTypeVector = 23, OpFunction = 54, OpIsNan = 156, OpSelect = 169,
           FMin = 37, FMax = 40, FClamp = 43, NMin = 79, NMax = 80, NClamp = 81 };
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3], glsl = 0, tbool = 0, bvec[5] = { 0 }; size_t hits = 0, extra = 0, first_fn = 0; int need[5] = { 0 };
    uint8_t *nc = calloc(bound ? bound : 1, 1); if (!nc) return NULL;   // per type id: 1 a 32-bit float, 2..4 a vector of them, 0 anything else
    #define NAN_OP(x, n) (glsl && (x)[3] == glsl && (x)[1] < bound && nc[(x)[1]] && ((((x)[4] == FMin || (x)[4] == FMax || (x)[4] == NMin || (x)[4] == NMax) && (n) == 7) || (((x)[4] == FClamp || (x)[4] == NClamp) && (n) == 8)))
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) { free(nc); return NULL; } const uint32_t *x = code + i;
        if (op == OpExtInstImport && n >= 3 && !strncmp((const char *)(x + 2), "GLSL.std.450", 4 * (n - 2))) glsl = x[1];
        else if (op == OpTypeBool && n == 2) tbool = x[1];
        else if (op == OpTypeFloat && n >= 3 && x[2] == 32 && x[1] < bound) nc[x[1]] = 1;
        else if (op == OpTypeVector && n == 4 && x[1] < bound && x[2] < bound && x[3] >= 2 && x[3] <= 4) { if (nc[x[2]] == 1) nc[x[1]] = (uint8_t)x[3]; else if (tbool && x[2] == tbool) bvec[x[3]] = x[1]; }
        else if (op == OpFunction && !first_fn) first_fn = i;
        else if (op == OpExtInst && n >= 7 && NAN_OP(x, n)) { hits++; need[nc[x[1]]] = 1; extra += n == 8 ? 10 : 20; }
        i += n; }
    if (!hits || !first_fn) { free(nc); return NULL; }
    uint32_t *o = malloc(4 * (words + extra + 2 + 3 * 4)); if (!o) { free(nc); return NULL; }
    uint32_t next = bound; size_t w = 0;
    for (size_t i = 0; i < words;) {
        size_t n = i < 5 ? 5 : code[i] >> 16; const uint32_t *x = code + i;
        if (i == first_fn) {   // the types the selects need, where the module has none
            if (!tbool) { tbool = next++; o[w++] = 2u << 16 | OpTypeBool; o[w++] = tbool; }
            for (uint32_t k = 2; k <= 4; k++) if (need[k] && !bvec[k]) { bvec[k] = next++; o[w++] = 4u << 16 | OpTypeVector; o[w++] = bvec[k]; o[w++] = tbool; o[w++] = k; }
        }
        memcpy(o + w, x, 4 * n);
        if (i >= 5 && (x[0] & 0xffff) == OpExtInst && n >= 7 && NAN_OP(x, n)) {
            uint32_t T = x[1], res = x[2], k = nc[T], B = k == 1 ? tbool : bvec[k], m = next++;
            o[w + 2] = m; w += n;
            #define ISNAN(id, v) (o[w++] = 4u << 16 | OpIsNan, o[w++] = B, o[w++] = (id), o[w++] = (v))
            #define SELECT(id, c, a, b) (o[w++] = 6u << 16 | OpSelect, o[w++] = T, o[w++] = (id), o[w++] = (c), o[w++] = (a), o[w++] = (b))
            if (n == 8) { uint32_t nx = next++; ISNAN(nx, x[5]); SELECT(res, nx, x[6], m); }
            else { uint32_t na = next++, nb = next++, t = next++; ISNAN(na, x[5]); ISNAN(nb, x[6]); SELECT(t, nb, x[5], m); SELECT(res, na, x[6], t); }
            #undef ISNAN
            #undef SELECT
        } else w += n;
        i += n; }
    #undef NAN_OP
    free(nc); o[3] = next; *out_words = w; return o;
}

// Maxwell's FMZ ("FMUL.FMZ", "FFMA.FMZ"): a product with a zero operand is 0, whatever the other operand is, infinity and NaN included. Switch shaders count on it (a
// light at the eye's own position, a length of zero normalised: 0 x inf), but the shader translator does not carry the flag over, and SPIR-V's OpFMul / Fma give NaN
// there, which reaches the picture as a white or glowing surface (Mali shows it; Galaxy-Android's Mali-G68 "glowing white" characters were the same divide by zero).
// A copy in which every 32-bit float multiply (OpFMul, scalar or vector) and GLSL.std.450 Fma has the rule written out: a == 0 or b == 0 -> 0 (Fma: c), else the op's
// own result, as OpFOrdEqual + OpLogicalOr + OpSelect (an operand that is NaN is not equal to 0, so 0 x NaN still gives 0). Every multiply, not only the ones the game's
// shader marked FMZ (the SPIR-V no longer says which): a test of the theory a switch away (DM_VK_FMZ), before the translator gets it per instruction and with it a new
// shader chain. The op keeps its place under a new id and the select takes its old id; bool / bvecN types and the zero constants are added before the first function.
// NULL when the module has no such op
uint32_t *spv_fmz(const uint32_t *code, size_t words, size_t *out_words) {
    enum { OpExtInstImport = 11, OpExtInst = 12, OpTypeBool = 20, OpTypeFloat = 22, OpTypeVector = 23, OpConstant = 43, OpConstantComposite = 44, OpFunction = 54,
           OpFMul = 133, OpLogicalOr = 166, OpSelect = 169, OpFOrdEqual = 180, Fma = 50 };
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3], glsl = 0, tbool = 0, bvec[5] = { 0 }; size_t hits = 0, extra = 0, first_fn = 0; int need[5] = { 0 };
    if (!bound || bound > (1u << 22)) return NULL;
    uint8_t *nc = calloc(bound, 1), *used = calloc(bound, 1); uint32_t *comp = calloc(bound, 4), *zid = calloc(bound, 4);   // per type id: 1 a 32-bit float, 2..4 a vector of them
    if (!nc || !used || !comp || !zid) { free(nc); free(used); free(comp); free(zid); return NULL; }
    // (the length first: x[1] of a one-word instruction is the next instruction's, or past the module for its last, OpFunctionEnd. Android's allocator puts a large
    //   block right before a guard page, so a big shader's read past its end crashed the game at load with FMZ on: Galar #172 #181, spv_fmz+0x1d8 on a page start)
    #define FMZ_OP(x, op, n) ((n) >= 5 && (x)[1] < bound && nc[(x)[1]] && (((op) == OpFMul && (n) == 5) || ((op) == OpExtInst && (n) == 8 && glsl && (x)[3] == glsl && (x)[4] == Fma)))
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) goto none; const uint32_t *x = code + i;
        if (op == OpExtInstImport && n >= 3 && !strncmp((const char *)(x + 2), "GLSL.std.450", 4 * (n - 2))) glsl = x[1];
        else if (op == OpTypeBool && n == 2) tbool = x[1];
        else if (op == OpTypeFloat && n >= 3 && x[2] == 32 && x[1] < bound) nc[x[1]] = 1;
        else if (op == OpTypeVector && n == 4 && x[1] < bound && x[2] < bound && x[3] >= 2 && x[3] <= 4) { if (nc[x[2]] == 1) { nc[x[1]] = (uint8_t)x[3]; comp[x[1]] = x[2]; } else if (tbool && x[2] == tbool) bvec[x[3]] = x[1]; }
        else if (op == OpFunction && !first_fn) first_fn = i;
        else if (first_fn && FMZ_OP(x, op, n)) { hits++; need[nc[x[1]]] = 1; used[x[1]] = 1; if (comp[x[1]]) used[comp[x[1]]] = 1; extra += 21; }
        i += n; }
    if (!hits || !first_fn) goto none;
    size_t ntypes = 0; for (uint32_t t = 0; t < bound; t++) ntypes += used[t];
    uint32_t *o = malloc(4 * (words + extra + 2 + 3 * 4 + ntypes * 8)); if (!o) goto none;
    uint32_t next = bound; size_t w = 0;
    for (size_t i = 0; i < words;) {
        size_t n = i < 5 ? 5 : code[i] >> 16; const uint32_t *x = code + i;
        if (i == first_fn) {   // the types and zeros the selects need
            if (!tbool) { tbool = next++; o[w++] = 2u << 16 | OpTypeBool; o[w++] = tbool; }
            for (uint32_t k = 2; k <= 4; k++) if (need[k] && !bvec[k]) { bvec[k] = next++; o[w++] = 4u << 16 | OpTypeVector; o[w++] = bvec[k]; o[w++] = tbool; o[w++] = k; }
            for (uint32_t t = 0; t < bound; t++) if (used[t] && nc[t] == 1) { zid[t] = next++; o[w++] = 4u << 16 | OpConstant; o[w++] = t; o[w++] = zid[t]; o[w++] = 0; }
            for (uint32_t t = 0; t < bound; t++) if (used[t] && nc[t] >= 2) { uint32_t k = nc[t]; zid[t] = next++; o[w++] = (3u + k) << 16 | OpConstantComposite; o[w++] = t; o[w++] = zid[t];
                for (uint32_t j = 0; j < k; j++) o[w++] = zid[comp[t]]; }
        }
        memcpy(o + w, x, 4 * n);
        uint32_t op = x[0] & 0xffff;
        if (i >= first_fn && first_fn && FMZ_OP(x, op, n)) {
            uint32_t T = x[1], res = x[2], k = nc[T], B = k == 1 ? tbool : bvec[k], m = next++, za = next++, zb = next++, z = next++, Z = zid[T];
            uint32_t a = op == OpFMul ? x[3] : x[5], b = op == OpFMul ? x[4] : x[6], other = op == OpFMul ? Z : x[7];   // (Fma: a, b, c)
            o[w + 2] = m; w += n;
            o[w++] = 5u << 16 | OpFOrdEqual; o[w++] = B; o[w++] = za; o[w++] = a; o[w++] = Z;
            o[w++] = 5u << 16 | OpFOrdEqual; o[w++] = B; o[w++] = zb; o[w++] = b; o[w++] = Z;
            o[w++] = 5u << 16 | OpLogicalOr; o[w++] = B; o[w++] = z; o[w++] = za; o[w++] = zb;
            o[w++] = 6u << 16 | OpSelect; o[w++] = T; o[w++] = res; o[w++] = z; o[w++] = other; o[w++] = m;
        } else w += n;
        i += n; }
    #undef FMZ_OP
    free(nc); free(used); free(comp); free(zid); o[3] = next; *out_words = w; return o;
none:
    free(nc); free(used); free(comp); free(zid); return NULL;
}

// Maxwell's special functions have exact answers where SPIR-V's are undefined: MUFU.LG2 gives -inf for 0 and NaN for a negative number, MUFU.EX2 0 below 2^-126 (it flushes
// the denormal) and +inf above 2^128, MUFU.RSQ +inf for 0 and NaN for a negative number, and F2I clamps to the integer's range with NaN giving 0. GLSL.std.450 Log2 / Exp2 /
// InverseSqrt / Sqrt and OpConvertFToU / FToS leave those cases to the driver, and a driver with fast approximations may answer anything there. The translated lighting
// takes pow (x, y) as exp2 (log2 (x) * y) of max (0, x): x is 0 on every surface facing away from a light, which Maxwell turns into exp2 (-inf) = 0; on Arm's own Mali driver
// the lit pass came out at the format's maximum on every Mali-G615 / G720 / G925 while PanVK drew the same shaders on the same phones (#47, v0.67 captures).
// A copy in which each scalar 32-bit float op of the classes in mask (EXACT_*) has Maxwell's answer written out with OpSelect; vectors are left as they are (the translator
// writes scalars). NULL when the module has none
uint32_t *spv_exact_math(const uint32_t *code, size_t words, size_t *out_words, unsigned mask) {
    enum { OpExtInstImport = 11, OpExtInst = 12, OpTypeBool = 20, OpTypeFloat = 22, OpConstant = 43, OpFunction = 54, OpConvertFToU = 109, OpConvertFToS = 110,
           OpIsNan = 156, OpSelect = 169, OpFOrdEqual = 180, OpFOrdLessThan = 184, OpFOrdGreaterThan = 186,
           Exp2 = 29, Log2 = 30, Sqrt = 31, InverseSqrt = 32, FClamp = 43 };
    enum { C_ZERO, C_NINF, C_PINF, C_NAN, C_M126, C_P128, C_U_HI, C_S_LO, C_S_HI, NC };
    static const uint32_t cval[NC] = { 0x00000000u, 0xff800000u, 0x7f800000u, 0x7fc00000u, 0xc2fc0000u, 0x43000000u, 0x4f7fffffu, 0xcf000000u, 0x4effffffu };
    if (words < 5 || code[0] != 0x07230203u || !mask) return NULL;
    uint32_t bound = code[3], glsl = 0, tbool = 0, tf32 = 0; size_t hits = 0, extra = 0, first_fn = 0;
    if (!bound || bound > (1u << 22)) return NULL;
    #define KIND(x, n) ((x)[0] & 0xffff) == OpExtInst && (n) == 6 && glsl && (x)[3] == glsl && tf32 && (x)[1] == tf32 ? \
        ((x)[4] == Log2 && (mask & EXACT_LOG2) ? 1 : (x)[4] == Exp2 && (mask & EXACT_EXP2) ? 2 : (x)[4] == InverseSqrt && (mask & EXACT_RSQ) ? 3 : (x)[4] == Sqrt && (mask & EXACT_SQRT) ? 4 : 0) : \
        (((x)[0] & 0xffff) == OpConvertFToU || ((x)[0] & 0xffff) == OpConvertFToS) && (n) == 4 && (mask & EXACT_FTOI) && glsl && tf32 ? 5 : 0
    uint8_t *isf = calloc(bound, 1); if (!isf) return NULL;   // ids whose type is the 32-bit float (operands of the conversions)
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) { free(isf); return NULL; } const uint32_t *x = code + i;
        if (op == OpExtInstImport && n >= 3 && !strncmp((const char *)(x + 2), "GLSL.std.450", 4 * (n - 2))) glsl = x[1];
        else if (op == OpTypeBool && n == 2) tbool = x[1];
        else if (op == OpTypeFloat && n >= 3 && x[2] == 32 && !tf32) tf32 = x[1];
        else if (op == OpFunction && !first_fn) first_fn = i;
        if (n >= 3 && op != OpFunction && x[1] == tf32 && tf32 && x[2] < bound && (op == OpConstant || op >= 61)) isf[x[2]] = 1;   // (results of type float; 61 = OpLoad onwards)
        int k = KIND(x, n);
        if (k == 5 && !(x[3] < bound && isf[x[3]])) k = 0;
        if (k) { hits++; extra += k <= 3 ? 22 : k == 4 ? 11 : 18; }   // (the words each rewrite adds)
        i += n; }
    if (!hits || !first_fn) { free(isf); return NULL; }
    uint32_t *o = malloc(4 * (words + extra + 2 + 4 * NC)); if (!o) { free(isf); return NULL; }
    uint32_t next = bound, c[NC]; size_t w = 0;
    for (size_t i = 0; i < words;) {
        size_t n = i < 5 ? 5 : code[i] >> 16; const uint32_t *x = code + i;
        if (i == first_fn) {   // bool where the module has none, and the constants
            if (!tbool) { tbool = next++; o[w++] = 2u << 16 | OpTypeBool; o[w++] = tbool; }
            for (int j = 0; j < NC; j++) { c[j] = next++; o[w++] = 4u << 16 | OpConstant; o[w++] = tf32; o[w++] = c[j]; o[w++] = cval[j]; }
        }
        int k = i >= 5 ? KIND(x, n) : 0;
        if (k == 5 && !(x[3] < bound && isf[x[3]])) k = 0;
        #define CMP(op, id, a, b) (o[w++] = 5u << 16 | (op), o[w++] = tbool, o[w++] = (id), o[w++] = (a), o[w++] = (b))
        #define SEL(id, cnd, a, b) (o[w++] = 6u << 16 | OpSelect, o[w++] = tf32, o[w++] = (id), o[w++] = (cnd), o[w++] = (a), o[w++] = (b))
        if (k >= 1 && k <= 4) {   // the op under a new id m, then Maxwell's answer for the edge cases under the op's own id
            uint32_t res = x[2], a = x[5], m = next++; memcpy(o + w, x, 4 * n); o[w + 2] = m; w += n;
            if (k == 1) { uint32_t z = next++, ng = next++, t = next++; CMP(OpFOrdEqual, z, a, c[C_ZERO]); CMP(OpFOrdLessThan, ng, a, c[C_ZERO]); SEL(t, ng, c[C_NAN], m); SEL(res, z, c[C_NINF], t); }
            else if (k == 2) { uint32_t lo = next++, hi = next++, t = next++; CMP(OpFOrdLessThan, lo, a, c[C_M126]); CMP(OpFOrdGreaterThan, hi, a, c[C_P128]); SEL(t, hi, c[C_PINF], m); SEL(res, lo, c[C_ZERO], t); }
            else if (k == 3) { uint32_t z = next++, ng = next++, t = next++; CMP(OpFOrdEqual, z, a, c[C_ZERO]); CMP(OpFOrdLessThan, ng, a, c[C_ZERO]); SEL(t, ng, c[C_NAN], m); SEL(res, z, c[C_PINF], t); }
            else { uint32_t ng = next++; CMP(OpFOrdLessThan, ng, a, c[C_ZERO]); SEL(res, ng, c[C_NAN], m); }
        } else if (k == 5) {   // the operand: NaN -> 0, then clamped to the integer's range, before the conversion
            uint32_t a = x[3], nn = next++, s = next++, cl = next++; int u = (x[0] & 0xffff) == OpConvertFToU;
            o[w++] = 4u << 16 | OpIsNan; o[w++] = tbool; o[w++] = nn; o[w++] = a;
            SEL(s, nn, c[C_ZERO], a);
            o[w++] = 8u << 16 | OpExtInst; o[w++] = tf32; o[w++] = cl; o[w++] = glsl; o[w++] = FClamp; o[w++] = s; o[w++] = u ? c[C_ZERO] : c[C_S_LO]; o[w++] = u ? c[C_U_HI] : c[C_S_HI];
            memcpy(o + w, x, 4 * n); o[w + 3] = cl; w += n;
        } else { memcpy(o + w, x, 4 * n); w += n; }
        #undef CMP
        #undef SEL
        i += n; }
    #undef KIND
    free(isf); o[3] = next; *out_words = w; return o;
}

// The translator declares every Maxwell register as a Function variable with no initializer; where a path reads a register it has not written, SPIR-V leaves the value
// undefined. Mesa's compilers turn such values into zero (Panfrost lowers undefs to 0: PanVK drew the lit scene right on a Mali-G615), while a driver may keep whatever the
// register held; the lit pass came out at the format's maximum on Arm's own Mali driver with every special-function edge case written out (#47, mali-white-test1).
// A copy in which every Function variable without an initializer starts at zero (OpConstantNull of its type, declared before the first function). NULL when there is none
uint32_t *spv_init_locals(const uint32_t *code, size_t words, size_t *out_words) {
    enum { OpTypePointer = 32, OpConstantNull = 46, OpFunction = 54, OpVariable = 59, Function = 7 };
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint32_t *pointee = calloc(bound, 4), *nul = calloc(bound, 4); if (!pointee || !nul) { free(pointee); free(nul); return NULL; }
    size_t hits = 0, first_fn = 0, ntypes = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) goto none; const uint32_t *x = code + i;
        if (op == OpTypePointer && n == 4 && x[1] < bound && x[2] == Function) pointee[x[1]] = x[3];
        else if (op == OpFunction && !first_fn) first_fn = i;
        else if (op == OpVariable && n == 4 && x[3] == Function && x[1] < bound && pointee[x[1]] && pointee[x[1]] < bound) {
            hits++; if (!nul[pointee[x[1]]]) { nul[pointee[x[1]]] = 1; ntypes++; } }
        i += n; }
    if (!hits || !first_fn) goto none;
    uint32_t *o = malloc(4 * (words + hits + 3 * ntypes)); if (!o) goto none;
    uint32_t next = bound; size_t w = 0;
    for (size_t i = 0; i < words;) {
        size_t n = i < 5 ? 5 : code[i] >> 16; const uint32_t *x = code + i;
        if (i == first_fn) for (uint32_t t = 0; t < bound; t++) if (nul[t] == 1) { nul[t] = next++; o[w++] = 3u << 16 | OpConstantNull; o[w++] = t; o[w++] = nul[t]; }
        if (i >= 5 && (x[0] & 0xffff) == OpVariable && n == 4 && x[3] == Function && x[1] < bound && pointee[x[1]] && pointee[x[1]] < bound && nul[pointee[x[1]]] > 1) {
            o[w++] = 5u << 16 | OpVariable; o[w++] = x[1]; o[w++] = x[2]; o[w++] = Function; o[w++] = nul[pointee[x[1]]];
        } else { memcpy(o + w, x, 4 * n); w += n; }
        i += n; }
    free(pointee); free(nul); o[3] = next; *out_words = w; return o;
none:
    free(pointee); free(nul); return NULL;
}

// Arm erratum 3779215 (Mali, Arm's own driver r38p1 to r49p0 and r50p0 to r51p0; fixed in r49p1 and r52p0): a min / max / clamp whose limit is a constant zero, of a value
// selected at run time between compile-time constants, is compiled as a plain min or max: the value is no longer clamped. A copy in which that zero is one the compiler cannot
// see as a constant: a Private variable per type, stored with the zero at the top of every entry point and loaded at the top of each function that uses it. Written from the
// erratum's description (MaliBridge MK-32's notes); tests: gfx/tests/t_clamp_zero.sh. NULL when the module has no such op
uint32_t *spv_clamp_zero(const uint32_t *code, size_t words, size_t *out_words) {
    enum { OpExtInstImport = 11, OpExtInst = 12, OpEntryPoint = 15, OpTypeFloat = 22, OpTypePointer = 32, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43,
           OpConstantComposite = 44, OpConstantNull = 46, OpFunction = 54, OpFunctionParameter = 55, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62,
           OpSelect = 169, OpLabel = 248, OpLine = 8, OpNoLine = 317, Private = 6, MAXT = 16, MAXEP = 16 };
    enum { K_CONST = 1, K_ZERO = 2, K_FLOAT = 4, K_SEL = 8 };
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint8_t *k = calloc(bound, 1), *fw = calloc(bound, 1); uint32_t *pp = calloc(bound, 4);   // what each id is; a float type's width in bytes; the Private pointer to a type
    typedef struct { size_t at; uint8_t mask, t; } hit;   // an op's position, the operands (x[5 + j]) to replace, its type's index in tl
    struct { uint32_t type, zero, ptr, var, load; } tl[MAXT]; int nt = 0;
    hit *h = NULL; size_t nh = 0, ch = 0; uint32_t glsl = 0, ep[MAXEP]; int nep = 0; size_t first_fn = 0;
    if (!k || !fw || !pp) goto none;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) goto none; const uint32_t *x = code + i;
        if (op == OpExtInstImport && n >= 3 && !strncmp((const char *)(x + 2), "GLSL.std.450", 4 * (n - 2))) glsl = x[1];
        else if (op == OpEntryPoint && n >= 3 && nep < MAXEP) ep[nep++] = x[2];
        else if (op == OpTypeFloat && n >= 3 && x[1] < bound) { k[x[1]] |= K_FLOAT; fw[x[1]] = (uint8_t)(x[2] / 8); }
        else if (op == OpTypePointer && n >= 4 && x[1] < bound && x[3] < bound && x[2] == Private && !pp[x[3]]) pp[x[3]] = x[1];
        else if ((op == OpConstant || op == OpConstantComposite || op == OpConstantNull || op == OpConstantTrue || op == OpConstantFalse) && n >= 3 && x[1] < bound && x[2] < bound) {
            int z = op == OpConstantNull;
            if (op == OpConstant) {   // zero, either sign for a float
                z = 1; for (uint32_t j = 3; j < n; j++) { uint32_t v = x[j];
                    if ((k[x[1]] & K_FLOAT) && j == n - 1) v &= fw[x[1]] == 2 ? 0x7fffu : 0x7fffffffu;
                    if (v) z = 0; } }
            if (op == OpConstantComposite) { z = n > 3; for (uint32_t j = 3; j < n; j++) if (x[j] >= bound || !(k[x[j]] & K_ZERO)) z = 0; }
            k[x[2]] |= K_CONST | (z ? K_ZERO : 0);
        }
        else if (op == OpSelect && n == 6 && x[2] < bound && x[4] < bound && x[5] < bound && (k[x[4]] & K_CONST) && (k[x[5]] & K_CONST)) k[x[2]] |= K_SEL;
        else if (op == OpFunction && !first_fn) first_fn = i;
        else if (op == OpExtInst && n >= 7 && x[3] == glsl && glsl) {
            uint32_t in = x[4], zero = 0; uint8_t mask = 0;
            #define ZERO(id) ((id) < bound && (k[id] & K_ZERO))
            #define SEL(id) ((id) < bound && (k[id] & K_SEL))
            if (n == 7 && ((in >= 37 && in <= 42) || in == 79 || in == 80)) {   // FMin UMin SMin FMax UMax SMax NMin NMax
                if (ZERO(x[5]) && SEL(x[6])) { mask |= 1; zero = x[5]; }
                if (ZERO(x[6]) && SEL(x[5])) { mask |= 2; zero = x[6]; } }
            else if (n == 8 && ((in >= 43 && in <= 45) || in == 81) && SEL(x[5])) {   // FClamp UClamp SClamp NClamp
                if (ZERO(x[6])) { mask |= 2; zero = x[6]; }
                if (ZERO(x[7])) { mask |= 4; zero = x[7]; } }
            #undef ZERO
            #undef SEL
            if (mask) {
                int t; for (t = 0; t < nt && tl[t].type != x[1]; t++) {}
                if (t == nt) { if (nt == MAXT) goto next; tl[nt].type = x[1]; tl[nt].zero = zero; tl[nt].ptr = x[1] < bound ? pp[x[1]] : 0; nt++; }
                if (nh == ch) { ch = ch ? ch * 2 : 16; hit *g = realloc(h, ch * sizeof *h); if (!g) goto none; h = g; }
                h[nh++] = (hit){ i, mask, (uint8_t)t };
            }
        }
      next:
        i += n; }
    if (!nh || !first_fn || !nep) goto none;
    {
    uint32_t next = bound; int iface = code[1] >= 0x10400;   // SPIR-V 1.4 and up: an entry point lists every global it uses, Private ones too
    for (int t = 0; t < nt; t++) { if (!tl[t].ptr) tl[t].ptr = next++ | 0x80000000u; tl[t].var = next++; }   // (high bit: a pointer type to declare)
    size_t cap = words + 64 + (size_t)nt * (8 + nep) + 8 * nh; uint32_t *o = malloc(4 * cap); if (!o) goto none;
    size_t w = 5, hi = 0; memcpy(o, code, 20);
    int in_fn = 0, is_ep = 0, done_top = 0; size_t fn_end = 0; uint8_t need[MAXT];
    #define ROOM(m) do { if (w + (m) > cap) { cap = cap * 2 + (m); uint32_t *o2 = realloc(o, 4 * cap); if (!o2) { free(o); goto none; } o = o2; } } while (0)
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (i == first_fn) { ROOM((size_t)nt * 8);   // the new globals, after every type and constant
            for (int t = 0; t < nt; t++) { if (tl[t].ptr & 0x80000000u) { tl[t].ptr &= 0x7fffffffu; o[w++] = 4u << 16 | OpTypePointer; o[w++] = tl[t].ptr; o[w++] = Private; o[w++] = tl[t].type; }
                o[w++] = 4u << 16 | OpVariable; o[w++] = tl[t].ptr; o[w++] = tl[t].var; o[w++] = Private; } }
        if (op == OpEntryPoint && iface) { ROOM(n + nt); o[w++] = (n + (uint32_t)nt) << 16 | op; memcpy(o + w, x + 1, 4 * (n - 1)); w += n - 1; for (int t = 0; t < nt; t++) o[w++] = tl[t].var; i += n; continue; }
        if (op == OpFunction) {
            in_fn = 1; done_top = 0; is_ep = 0; for (int e = 0; e < nep; e++) if (ep[e] == x[2]) is_ep = 1;
            for (fn_end = i; fn_end < words && (code[fn_end] & 0xffff) != OpFunctionEnd; fn_end += code[fn_end] >> 16) if (!(code[fn_end] >> 16)) break;
            memset(need, 0, sizeof need); for (size_t q = hi; q < nh && h[q].at < fn_end; q++) need[h[q].t] = 1;
        }
        else if (in_fn && !done_top && op != OpLabel && op != OpVariable && op != OpFunctionParameter && op != OpLine && op != OpNoLine) {   // the first block, after its variables
            ROOM((size_t)nt * 7); done_top = 1;
            if (is_ep) for (int t = 0; t < nt; t++) { o[w++] = 3u << 16 | OpStore; o[w++] = tl[t].var; o[w++] = tl[t].zero; }
            for (int t = 0; t < nt; t++) if (need[t]) { tl[t].load = next++; o[w++] = 4u << 16 | OpLoad; o[w++] = tl[t].type; o[w++] = tl[t].load; o[w++] = tl[t].var; }
        }
        if (op == OpFunctionEnd) in_fn = 0;
        ROOM(n); memcpy(o + w, x, 4 * n);
        if (hi < nh && h[hi].at == i) { for (int j = 0; j < 3; j++) if (h[hi].mask >> j & 1) o[w + 5 + j] = tl[h[hi].t].load; hi++; }
        w += n; i += n;
    }
    #undef ROOM
    o[3] = next; *out_words = w;
    free(k); free(fw); free(pp); free(h);
    return o;
    }
  none:
    free(k); free(fw); free(pp); free(h);
    return NULL;
}

// A diagnostic for the white 3D on Arm's own Mali driver (#215 and the issues it lists): every value of one kind in a fragment shader passed through NClamp, so a
// run with each kind tells which one carries the overflow (a picture that stops being white names it). The kinds (mask): PROBE_SAMPLES every texture sample and fetch
// (clamped to [-1024, 1024]), PROBE_INPUTS every float the previous stage hands over (Input loads, [-16384, 16384]), PROBE_DIV every float division ([-16384, 16384]),
// PROBE_OUTPUT what is stored to an output ([-64, 64]), and the arithmetic one kind at a time ([-16384, 16384]): PROBE_MUL (FMul, VectorTimesScalar), PROBE_ADD
// (FAdd, FSub), PROBE_FMA (GLSL Fma). Not a probe but a fix: NAN_FIX_OUT turns a NaN stored to an output into 0 (infinities into ±65504, other values kept), NAN_FIX_TEX
// the same on every texture sample, so a NaN written by one pass can't spread through the passes that read it (#215: NaN reaches the lit scene's target)
// NClamp also turns a NaN into the low bound. The result keeps its id: each op is emitted under a new one and the clamp takes the old. NULL when nothing matched
uint32_t *spv_probe(const uint32_t *code, size_t words, size_t *out_words, unsigned mask) {
    enum { OpExtInstImport = 11, OpExtInst = 12, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpConstant = 43, OpConstantComposite = 44, OpFunction = 54,
           OpVariable = 59, OpLoad = 61, OpStore = 62, OpFDiv = 136, NClamp = 81, Input = 1, Output = 3 };
    if (words < 5 || code[0] != 0x07230203u || !mask) return NULL;
    uint32_t bound = code[3], glsl = 0, tf32 = 0; size_t first_fn = 0, hits = 0;
    if (!bound || bound > (1u << 22)) return NULL;
    uint8_t *width = calloc(bound, 1), *inptr = calloc(bound, 1), *invar = calloc(bound, 1);   // float types by id (1 scalar, 2-4 vectors); Input pointers; Input variables
    uint32_t *outptr = calloc(bound, 4), *outty = calloc(bound, 4);   // Output pointer types → their pointee; Output variables / chains into them → the type stored
    if (!width || !inptr || !invar || !outptr || !outty) { free(width); free(inptr); free(invar); free(outptr); free(outty); return NULL; }
    #define KIND(x, n, op) ( \
        ((mask & PROBE_SAMPLES) && (op) >= 87 && (op) <= 96 && (n) >= 5 && (x)[1] < bound && width[(x)[1]]) ? 1 : \
        ((mask & PROBE_INPUTS) && (op) == OpLoad && (n) >= 4 && (x)[1] < bound && width[(x)[1]] && (x)[3] < bound && invar[(x)[3]]) ? 2 : \
        ((mask & PROBE_DIV) && (op) == OpFDiv && (n) == 5 && (x)[1] < bound && width[(x)[1]]) ? 3 : \
        ((mask & PROBE_OUTPUT) && (op) == OpStore && (n) >= 3 && (x)[1] < bound && outty[(x)[1]] && outty[(x)[1]] < bound && width[outty[(x)[1]]]) ? 4 : \
        ((mask & PROBE_MUL) && ((op) == 133 || (op) == 142) && (n) == 5 && (x)[1] < bound && width[(x)[1]]) ? 5 : \
        ((mask & PROBE_ADD) && ((op) == 129 || (op) == 131) && (n) == 5 && (x)[1] < bound && width[(x)[1]]) ? 6 : \
        ((mask & PROBE_FMA) && (op) == OpExtInst && (n) == 8 && glsl && (x)[3] == glsl && (x)[4] == 50 && (x)[1] < bound && width[(x)[1]]) ? 7 : \
        ((mask & NAN_FIX_OUT) && (op) == OpStore && (n) >= 3 && (x)[1] < bound && outty[(x)[1]] && outty[(x)[1]] < bound && width[outty[(x)[1]]]) ? 8 : \
        ((mask & NAN_FIX_TEX) && (op) >= 87 && (op) <= 96 && (n) >= 5 && (x)[1] < bound && width[(x)[1]]) ? 9 : 0)
    for (int pass = 0; pass < 2; pass++)
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) goto none; const uint32_t *x = code + i;
        if (!pass) {
            if (op == OpExtInstImport && n >= 3 && !strncmp((const char *)(x + 2), "GLSL.std.450", 4 * (n - 2))) glsl = x[1];
            else if (op == OpTypeFloat && n >= 3 && x[2] == 32 && x[1] < bound) { if (!tf32) tf32 = x[1]; if (x[1] == tf32) width[x[1]] = 1; }
            else if (op == OpTypeVector && n == 4 && x[1] < bound && tf32 && x[2] == tf32 && x[3] >= 2 && x[3] <= 4) width[x[1]] = (uint8_t)x[3];
            else if (op == OpTypePointer && n == 4 && x[1] < bound && x[2] == Input) inptr[x[1]] = 1;
            else if (op == OpVariable && n >= 4 && x[2] < bound && x[1] < bound && inptr[x[1]]) invar[x[2]] = 1;
            else if ((op == 65 || op == 66) && n >= 4 && x[2] < bound && x[3] < bound && invar[x[3]]) invar[x[2]] = 1;   // OpAccessChain / OpInBoundsAccessChain into an input
            if (op == OpTypePointer && n == 4 && x[1] < bound && x[2] == Output) outptr[x[1]] = x[3];
            if (op == OpVariable && n >= 4 && x[2] < bound && x[1] < bound && outptr[x[1]]) outty[x[2]] = outptr[x[1]];
            if ((op == 65 || op == 66) && n >= 4 && x[2] < bound && x[3] < bound && outty[x[3]] && x[1] < bound && outptr[x[1]]) outty[x[2]] = outptr[x[1]];
            else if (op == OpFunction && !first_fn) first_fn = i;
        } else if (KIND(x, n, op)) hits++;
        i += n; }
    if (!hits || !glsl || !tf32 || !first_fn) goto none;
    {   // constants: lo / hi per kind and width
        static const uint32_t lo_bits[10] = { 0, 0xc4800000u, 0xc6800000u, 0xc6800000u, 0xc2800000u, 0xc6800000u, 0xc6800000u, 0xc6800000u, 0xc77fe000u, 0xc77fe000u },
                              hi_bits[10] = { 0, 0x44800000u, 0x46800000u, 0x46800000u, 0x42800000u, 0x46800000u, 0x46800000u, 0x46800000u, 0x477fe000u, 0x477fe000u };   // ±1024, ±16384, output ±64, arithmetic ±16384, NaN fix ±65504
        uint32_t *o = malloc(4 * (words + hits * 56 + 64)); if (!o) goto none;   // (each: 8 words of clamp, up to 14 of vector bounds; a NaN fix up to 48)
        uint32_t next = bound, cs[10][5][2], zero = 0; memset(cs, 0, sizeof cs); size_t w = 0;
        // NaN fix: v -> NClamp(NMax(v, 0) + NMin(v, 0), -65504, 65504). NMax / NMin return the other operand for a NaN, so a NaN becomes 0 + 0, a finite value
        //   v + 0, and an infinity the half-float limit (no bool types needed, so nothing to add to the globals)
        #define SCRUB(ty, v, out, k) do { uint32_t wd_ = width[ty], z_ = zero, lo_ = cs[k][1][0], hi_ = cs[k][1][1], a_ = next++, b_ = next++, s_ = next++; \
            if (wd_ > 1) { z_ = next++; lo_ = next++; hi_ = next++; \
                o[w++] = (3u + wd_) << 16 | 80; o[w++] = ty; o[w++] = z_; for (uint32_t q = 0; q < wd_; q++) o[w++] = zero; \
                o[w++] = (3u + wd_) << 16 | 80; o[w++] = ty; o[w++] = lo_; for (uint32_t q = 0; q < wd_; q++) o[w++] = cs[k][1][0]; \
                o[w++] = (3u + wd_) << 16 | 80; o[w++] = ty; o[w++] = hi_; for (uint32_t q = 0; q < wd_; q++) o[w++] = cs[k][1][1]; } \
            o[w++] = 7u << 16 | OpExtInst; o[w++] = ty; o[w++] = a_; o[w++] = glsl; o[w++] = 80 /* NMax */; o[w++] = (v); o[w++] = z_; \
            o[w++] = 7u << 16 | OpExtInst; o[w++] = ty; o[w++] = b_; o[w++] = glsl; o[w++] = 79 /* NMin */; o[w++] = (v); o[w++] = z_; \
            o[w++] = 5u << 16 | 129 /* OpFAdd */; o[w++] = ty; o[w++] = s_; o[w++] = a_; o[w++] = b_; \
            o[w++] = 8u << 16 | OpExtInst; o[w++] = ty; o[w++] = (out); o[w++] = glsl; o[w++] = NClamp; o[w++] = s_; o[w++] = lo_; o[w++] = hi_; } while (0)
        for (size_t i = 0; i < words;) {
            size_t n = i < 5 ? 5 : code[i] >> 16; const uint32_t *x = code + i; uint32_t op = i < 5 ? 0 : x[0] & 0xffff;
            if (i == first_fn) { for (int k = 1; k <= 9; k++) {
                uint32_t sl = next++, sh = next++; o[w++] = 4u << 16 | OpConstant; o[w++] = tf32; o[w++] = sl; o[w++] = lo_bits[k]; o[w++] = 4u << 16 | OpConstant; o[w++] = tf32; o[w++] = sh; o[w++] = hi_bits[k];
                cs[k][1][0] = sl; cs[k][1][1] = sh; }
                zero = next++; o[w++] = 4u << 16 | OpConstant; o[w++] = tf32; o[w++] = zero; o[w++] = 0; }
            int k = i >= 5 ? KIND(x, n, op) : 0;
            if (k == 8) {   // NaN fix on the value stored to an output; the store takes the fixed value
                uint32_t c = next++; SCRUB(outty[x[1]], x[2], c, 8); memcpy(o + w, x, 4 * n); o[w + 2] = c; w += n;
            } else if (k == 9) {   // NaN fix on a texture sample: the sample under a new id, the fix under the old
                uint32_t ty = x[1], res = x[2], m = next++; memcpy(o + w, x, 4 * n); o[w + 2] = m; w += n; SCRUB(ty, m, res, 9);
            } else if (k == 4) {   // the value stored to an output, clamped first; the store then takes the clamp
                uint32_t ty = outty[x[1]], wd = width[ty], lo = cs[4][1][0], hi = cs[4][1][1], c = next++;
                if (wd > 1) { uint32_t vl = next++, vh = next++;
                    o[w++] = (3u + wd) << 16 | 80; o[w++] = ty; o[w++] = vl; for (uint32_t q = 0; q < wd; q++) o[w++] = cs[4][1][0];
                    o[w++] = (3u + wd) << 16 | 80; o[w++] = ty; o[w++] = vh; for (uint32_t q = 0; q < wd; q++) o[w++] = cs[4][1][1];
                    lo = vl; hi = vh; }
                o[w++] = 8u << 16 | OpExtInst; o[w++] = ty; o[w++] = c; o[w++] = glsl; o[w++] = NClamp; o[w++] = x[2]; o[w++] = lo; o[w++] = hi;
                memcpy(o + w, x, 4 * n); o[w + 2] = c; w += n;
            } else if (k) {
                uint32_t ty = x[1], res = x[2], m = next++, wd = width[ty]; memcpy(o + w, x, 4 * n); o[w + 2] = m; w += n;
                uint32_t lo = cs[k][1][0], hi = cs[k][1][1];
                if (wd > 1) {   // vector bounds: composites built where the result is (after the type exists), once per type and kind would need the globals section; built in place
                    uint32_t vl = next++, vh = next++;
                    o[w++] = (3u + wd) << 16 | 80 /* OpCompositeConstruct */; o[w++] = ty; o[w++] = vl; for (uint32_t c = 0; c < wd; c++) o[w++] = cs[k][1][0];
                    o[w++] = (3u + wd) << 16 | 80; o[w++] = ty; o[w++] = vh; for (uint32_t c = 0; c < wd; c++) o[w++] = cs[k][1][1];
                    lo = vl; hi = vh; }
                o[w++] = 8u << 16 | OpExtInst; o[w++] = ty; o[w++] = res; o[w++] = glsl; o[w++] = NClamp; o[w++] = m; o[w++] = lo; o[w++] = hi;
            } else { memcpy(o + w, x, 4 * n); w += n; }
            i += n; }
        free(width); free(inptr); free(invar); free(outptr); free(outty); o[3] = next; *out_words = w; return o;
    }
none:
    free(width); free(inptr); free(invar); free(outptr); free(outty); return NULL;
    #undef KIND
    #undef SCRUB
}

// ---- SPIR-V 1.0, for Vulkan 1.0 drivers ----
// The translator writes SPIR-V 1.3 and declares capabilities every shader of a stage gets: the subgroup ones (GroupNonUniform, Vote, Ballot, Shuffle), which only Vulkan 1.1
//   has. Of 1,543 Sword / Shield shaders every one declared them and none used a subgroup instruction; nothing else in them is newer than SPIR-V 1.0 but draw parameters
//   (an extension on Vulkan 1.0). A copy here: version 1.0, the unused subgroup capabilities dropped, OpExtension "SPV_KHR_shader_draw_parameters" added where DrawParameters is
//   declared and "SPV_KHR_storage_buffer_storage_class" where that storage class is used. NULL when the module really uses something 1.0 cannot have (a subgroup instruction or
//   built-in, an instruction from a later version), or is already 1.0
static size_t spv_str_words(const char *s) { return (strlen(s) + 4) / 4; }
uint32_t *spv_to_10(const uint32_t *code, size_t words, size_t *out_words) {
    if (words < 5 || code[0] != 0x07230203u || code[1] <= 0x00010000u) return NULL;
    int drawp = 0, sbsc = 0, has_dp_ext = 0, has_sb_ext = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return NULL; const uint32_t *x = code + i;
        if (op >= 333 && op <= 366) return NULL;                                   // OpGroupNonUniform*: really used
        if (op == 330 || op == 331 || op == 332 || (op >= 400 && op <= 403) || op == 4421 || op == 4428) return NULL;   // instructions from SPIR-V 1.1+ / subgroup extensions
        if (op == OpDecorate && n >= 4 && x[2] == 11 /* BuiltIn */ && ((x[3] >= 36 && x[3] <= 41) || (x[3] >= 4416 && x[3] <= 4420))) return NULL;   // subgroup built-ins
        if (op == 17 && n == 2 && x[1] == 4427) drawp = 1;                          // DrawParameters
        if (op == OpVariable && n >= 4 && x[3] == 12) sbsc = 1;                      // StorageBuffer storage class
        if (op == OpTypePointer && n >= 4 && x[2] == 12) sbsc = 1;
        if (op == 10 && n >= 2) { const char *s = (const char *)(x + 1);
            if (!strncmp(s, "SPV_KHR_shader_draw_parameters", 4 * (n - 1))) has_dp_ext = 1;
            if (!strncmp(s, "SPV_KHR_storage_buffer_storage_class", 4 * (n - 1))) has_sb_ext = 1; }
        i += n; }
    const char *add[2]; int nadd = 0;
    if (drawp && !has_dp_ext) add[nadd++] = "SPV_KHR_shader_draw_parameters";
    if (sbsc && !has_sb_ext) add[nadd++] = "SPV_KHR_storage_buffer_storage_class";
    size_t extra = 0; for (int k = 0; k < nadd; k++) extra += 1 + spv_str_words(add[k]);
    uint32_t *o = malloc(4 * (words + extra)); if (!o) return NULL;
    memcpy(o, code, 20); o[1] = 0x00010000u;
    size_t w = 5; int added = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (!added && op != 17) {   // the new extensions right after the last OpCapability (OpExtension comes next in the layout)
            for (int k = 0; k < nadd; k++) { size_t sw = spv_str_words(add[k]); o[w] = (uint32_t)(1 + sw) << 16 | 10; memset(o + w + 1, 0, 4 * sw); memcpy(o + w + 1, add[k], strlen(add[k])); w += 1 + sw; }
            added = 1; }
        int drop = op == 17 && n == 2 && ((x[1] >= 61 && x[1] <= 66) || x[1] == 4423 || x[1] == 4431);   // the subgroup capabilities, unused (checked above)
        if (!drop) { memcpy(o + w, x, 4 * n); w += n; }
        i += n; }
    *out_words = w; return o;
}

// ---- Devices without draw parameters (Vulkan 1.0 without VK_KHR_shader_draw_parameters; Vulkan 1.1+ with shaderDrawParameters off: Arm's Mali drivers among them) ----
// The DrawParameters capability and its extension are dropped. A shader that reads BaseVertex / BaseInstance / DrawIndex (144 of 36,600 Sword / Shield vertex shaders read
//   BaseInstance) reads them instead from push constants at SPV_DRAWP_PC_OFFSET (three 32-bit words: base vertex, base instance, draw index; the draw pushes them, nvn_vk.c
//   dp_push). A module that already has push constants (a transform feedback variant) reads 0. *mode: 0 the capability dropped only, 1 push constants, 2 zero. NULL: nothing
//   to do (no DrawParameters) or not possible (the built-ins of different types, or used other than loaded)
uint32_t *spv_no_drawparams(const uint32_t *code, size_t words, size_t *out_words, int *mode) {
    enum { OpCapability = 17, OpExtension = 10, OpMemberDecorate = 72, OpLoad = 61, OpFunction = 54, OpCopyObject = 83, SC_PushConstant = 9, DecOffset = 35 };
    *mode = 0;
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint8_t *kind = calloc(bound, 1); if (!kind) return NULL;   // a draw-parameter variable: 1 + (0 BaseVertex / 1 BaseInstance / 2 DrawIndex)
    int cap = 0, has_pc = 0, ndp = 0, bad = 0; size_t first_fn = 0; uint32_t tm = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) { free(kind); return NULL; } const uint32_t *x = code + i;
        if (op == OpCapability && n == 2 && x[1] == 4427) cap = 1;
        else if (op == OpDecorate && n >= 4 && x[1] < bound && x[2] == DecBuiltIn && x[3] >= 4424 && x[3] <= 4426) { kind[x[1]] = (uint8_t)(1 + x[3] - 4424); ndp++; }
        else if (op == OpVariable && n >= 4 && x[3] == SC_PushConstant) has_pc = 1;
        else if (op == OpFunction && !first_fn) first_fn = i;
        i += n; }
    if (!cap || !first_fn) { free(kind); return NULL; }
    if (ndp) for (size_t i = 5; i < first_fn;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;   // each one's type: a 32-bit integer, the same for all
        if (op == OpVariable && n >= 4 && x[2] < bound && kind[x[2]]) { const uint32_t *p = def_of(code, words, x[1]); const uint32_t *t = p ? def_of(code, words, p[3]) : NULL;
            if (!t || (t[0] & 0xffff) != OpTypeInt || t[2] != 32 || (tm && tm != p[3])) bad = 1; else tm = p[3]; }
        i += n; }
    if (ndp && !tm) bad = 1;
    // used only by OpLoad (any other instruction naming one of them: not possible)
    if (ndp && !bad) for (size_t i = first_fn; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        for (uint32_t k = 1; k < n && !bad; k++) if (x[k] < bound && kind[x[k]] && !(op == OpLoad && k == 3)) bad = 1;
        i += n; }
    if (bad) { free(kind); return NULL; }
    int m = !ndp ? 0 : has_pc ? 2 : 1; *mode = m;
    uint32_t nb = bound, t_s = 0, t_ps = 0, t_pt = 0, v_p = 0, c[3] = {0}, cz = 0;
    if (m == 1) { t_s = nb++; t_ps = nb++; t_pt = nb++; v_p = nb++; for (int k = 0; k < 3; k++) c[k] = nb++; }
    if (m == 2) cz = nb++;
    size_t nload = 0; for (size_t i = first_fn; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (op == OpLoad && n >= 4 && code[i + 3] < bound && kind[code[i + 3]]) nload++; i += n; }
    uint32_t *o = malloc(4 * (words + 64 + nload * 5)); if (!o) { free(kind); return NULL; }
    memcpy(o, code, 20); size_t w = 5; int dec = 0, decl = 0, in_fn = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpFunction) in_fn = 1;
        if (m == 1 && !dec && !in_fn && ((op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == OpVariable)) {   // the block's decorations, after the other annotations
            o[w++] = 3u << 16 | OpDecorate; o[w++] = t_s; o[w++] = DecBlock;
            for (uint32_t k = 0; k < 3; k++) { o[w++] = 5u << 16 | OpMemberDecorate; o[w++] = t_s; o[w++] = k; o[w++] = DecOffset; o[w++] = SPV_DRAWP_PC_OFFSET + 4 * k; }
            dec = 1; }
        if (!decl && op == OpFunction) {   // the new types, constants and the variable, before the first function
            if (m == 1) { o[w++] = 5u << 16 | OpTypeStruct; o[w++] = t_s; o[w++] = tm; o[w++] = tm; o[w++] = tm;
                          o[w++] = 4u << 16 | OpTypePointer; o[w++] = t_ps; o[w++] = SC_PushConstant; o[w++] = t_s;
                          o[w++] = 4u << 16 | OpTypePointer; o[w++] = t_pt; o[w++] = SC_PushConstant; o[w++] = tm;
                          for (uint32_t k = 0; k < 3; k++) { o[w++] = 4u << 16 | OpConstant; o[w++] = tm; o[w++] = c[k]; o[w++] = k; }
                          o[w++] = 4u << 16 | OpVariable; o[w++] = t_ps; o[w++] = v_p; o[w++] = SC_PushConstant; }
            if (m == 2) { o[w++] = 4u << 16 | OpConstant; o[w++] = tm; o[w++] = cz; o[w++] = 0; }
            decl = 1; }
        if (op == OpCapability && n == 2 && x[1] == 4427) { i += n; continue; }
        if (op == OpExtension && n >= 2 && !strncmp((const char *)(x + 1), "SPV_KHR_shader_draw_parameters", 4 * (n - 1))) { i += n; continue; }
        if ((op == OpName || op == OpDecorate) && n >= 2 && x[1] < bound && kind[x[1]]) { i += n; continue; }
        if (op == OpVariable && !in_fn && n >= 4 && x[2] < bound && kind[x[2]]) { i += n; continue; }
        if (op == OpEntryPoint && n >= 4) {   // the interface without them (a push constant variable is listed there only from SPIR-V 1.4 on)
            size_t sw = (strnlen((const char *)(x + 3), 4 * (n - 3)) + 4) / 4, hw = w; o[w++] = 0;
            for (uint32_t k = 1; k < 3 + sw && k < n; k++) o[w++] = x[k];
            for (uint32_t k = (uint32_t)(3 + sw); k < n; k++) if (!(x[k] < bound && kind[x[k]])) o[w++] = x[k];
            if (m == 1 && code[1] >= 0x00010400u) o[w++] = v_p;
            o[hw] = (uint32_t)(w - hw) << 16 | OpEntryPoint; i += n; continue; }
        if (op == OpLoad && n >= 4 && x[3] < bound && kind[x[3]]) {
            if (m == 1) { uint32_t t = nb++; o[w++] = 5u << 16 | OpAccessChain; o[w++] = t_pt; o[w++] = t; o[w++] = v_p; o[w++] = c[kind[x[3]] - 1];
                          o[w++] = 4u << 16 | OpLoad; o[w++] = x[1]; o[w++] = x[2]; o[w++] = t; }
            else { o[w++] = 4u << 16 | OpCopyObject; o[w++] = x[1]; o[w++] = x[2]; o[w++] = cz; }
            i += n; continue; }
        memcpy(o + w, x, 4 * n); w += n; i += n;
    }
    free(kind);
    if ((m == 1 && (!dec || !decl)) || (m == 2 && !decl)) { free(o); *mode = 0; return NULL; }
    o[3] = nb; *out_words = w; return o;
}

// Vulkan 1.0 without VK_KHR_storage_buffer_storage_class: storage buffers in the StorageBuffer storage class (the transform feedback capture variants of SPIR-V 1.3
//   shaders) become Uniform + BufferBlock, the form every Vulkan 1.0 driver takes. In place. 1 changed, 0 nothing to do, -1 not possible (a struct used both ways)
int spv_sb_to_uniform(uint32_t *code, size_t words) {
    if (words < 5 || code[0] != 0x07230203u) return 0;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return 0;
    uint8_t *st = calloc(bound, 1); if (!st) return -1;   // struct ids: 1 behind a StorageBuffer pointer, 2 behind any other pointer
    int any = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) { free(st); return -1; } const uint32_t *x = code + i;
        if (op == OpTypePointer && n >= 4 && x[3] < bound) { if (x[2] == SC_StorageBuffer) { st[x[3]] |= 1; any = 1; } else st[x[3]] |= 2; }
        i += n; }
    if (!any) { free(st); return 0; }
    for (uint32_t k = 0; k < bound; k++) if (st[k] == 3) { free(st); return -1; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; uint32_t *x = code + i;
        if (op == OpTypePointer && n >= 4 && x[2] == SC_StorageBuffer) x[2] = SC_Uniform;
        else if (op == OpVariable && n >= 4 && x[3] == SC_StorageBuffer) x[3] = SC_Uniform;
        else if (op == OpDecorate && n >= 3 && x[1] < bound && st[x[1]] == 1 && x[2] == DecBlock) x[2] = DecBufferBlock;
        i += n; }
    free(st); return 1;
}

// What a shader needs of the device's optional features (Vulkan 1.0 core features a driver may lack): SPV_NEED_* bits
uint32_t spv_feature_needs(const uint32_t *code, size_t words) {
    if (words < 5 || code[0] != 0x07230203u) return 0;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return 0;
    uint32_t need = 0; int frag = 0, fstore = 0;
    uint8_t *sc = calloc(bound, 1); uint32_t *ty = calloc(bound, 4);   // sc: a pointer type's storage class + 1; ty: a pointer id's type
    if (!sc || !ty) { free(sc); free(ty); return 0; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == 17 && n == 2) switch (x[1]) {
            case 2: need |= SPV_NEED_GEOM; break; case 3: need |= SPV_NEED_TESS; break; case 10: need |= SPV_NEED_F64; break; case 11: need |= SPV_NEED_I64; break;
            case 22: need |= SPV_NEED_I16; break; case 23: case 24: need |= SPV_NEED_TGPSIZE; break; case 25: need |= SPV_NEED_GATHEREXT; break;
            case 27: need |= SPV_NEED_STORMS; break; case 32: need |= SPV_NEED_CLIP; break; case 33: need |= SPV_NEED_CULL; break;
            case 34: case 45: need |= SPV_NEED_CUBEARR; break; case 35: need |= SPV_NEED_SRS; break; case 41: need |= SPV_NEED_SPARSE; break;
            case 42: need |= SPV_NEED_MINLOD; break; case 49: need |= SPV_NEED_EXTFMT; break; case 55: need |= SPV_NEED_IMGRNF; break; case 56: need |= SPV_NEED_IMGWNF; break;
            case 57: need |= SPV_NEED_MVP; break; default: break; }
        else if (op == OpEntryPoint && n >= 2 && x[1] == 4) frag = 1;
        else if (op == OpTypePointer && n >= 4 && x[1] < bound) sc[x[1]] = (uint8_t)(x[2] + 1);
        else if ((op == OpVariable || op == 65 || op == 66 || op == 67) && n >= 3 && x[2] < bound) ty[x[2]] = x[1];   // (and the access chains)
        else if (op == 62 /* OpStore */ && n >= 3 && x[1] < bound && ty[x[1]] < bound) { uint8_t s = sc[ty[x[1]]]; if (s == 1 + SC_Uniform || s == 1 + SC_StorageBuffer) fstore = 1; }
        else if (op == 99 /* OpImageWrite */ || (op >= 227 && op <= 242)) fstore = 1;   // (and the atomics)
        i += n; }
    free(sc); free(ty);
    if (frag && fstore) need |= SPV_NEED_FRAGSTORE;
    return need;
}

// ---- Vulkan 1.0 without VK_KHR_maintenance1: no negative viewport height, so NVN's upper-left origin is made in the vertex shader instead ----
// Before every return of the vertex entry point: gl_Position.y *= s, s a float specialization constant (SPV_FLIP_Y_SPEC_ID, default 1.0) that the pipeline sets to -1 while the
// game's window origin is upper left. y_ndc -> -y_ndc under a positive viewport puts every pixel where the flipped viewport did (same rows, same winding, same gl_FragCoord)
uint32_t *spv_flip_y(const uint32_t *code, size_t words, size_t *out_words) {
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint32_t entry = 0, posvar = 0, blk = 0, mem = 0, f32 = 0, v4 = 0, i32 = 0, pv4 = 0, cmem = 0;
    int nret = 0, vertex = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return NULL; const uint32_t *x = code + i;
        if (op == OpEntryPoint && n >= 3 && !entry) { entry = x[2]; vertex = x[1] == 0; }
        else if (op == OpDecorate && n >= 4 && x[2] == DecBuiltIn && x[3] == 0 /* Position */) posvar = x[1];
        else if (op == 72 /* OpMemberDecorate */ && n >= 5 && x[3] == DecBuiltIn && x[4] == 0) { blk = x[1]; mem = x[2]; }
        i += n; }
    if (!vertex || !entry || (!posvar && !blk)) return NULL;
    if (blk) {   // the gl_PerVertex block: its Output variable, and the member's type
        const uint32_t *s = def_of(code, words, blk); if (!s || (s[0] & 0xffff) != OpTypeStruct || 2 + mem >= (s[0] >> 16)) return NULL;
        v4 = s[2 + mem];
        for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
            if (op == OpVariable && n >= 4 && x[3] == SC_Output) { const uint32_t *p = def_of(code, words, x[1]); if (p && p[3] == blk) posvar = x[2]; }
            i += n; }
        if (!posvar) return NULL;
    } else {
        for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
            if (op == OpVariable && n >= 4 && x[2] == posvar) { const uint32_t *p = def_of(code, words, x[1]); if (p) v4 = p[3]; }
            i += n; }
    }
    const uint32_t *v = def_of(code, words, v4); if (!v || (v[0] & 0xffff) != OpTypeVector || v[3] != 4) return NULL;
    f32 = v[2];
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpTypePointer && n >= 4 && x[2] == SC_Output && x[3] == v4 && !pv4) pv4 = x[1];
        if (op == OpTypeInt && n >= 4 && x[2] == 32 && !i32) i32 = x[1];
        i += n; }
    if (blk && i32) for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpConstant && n == 4 && x[1] == i32 && x[3] == mem) { cmem = x[2]; break; }
        i += n; }
    for (size_t i = 5, in = 0; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (op == 54 /* OpFunction */) in = code[i + 2] == entry; else if (op == 253 /* OpReturn */ && in) nret++;
        i += n; }
    if (!nret) return NULL;
    uint32_t nb = bound, spec = nb++;
    int add_i32 = blk && !i32, add_c = blk && !cmem, add_p = blk && !pv4;
    if (add_i32) i32 = nb++;
    if (add_c) cmem = nb++;
    if (add_p) pv4 = nb++;
    size_t cap = words + 4 + 16 + (size_t)nret * 28; uint32_t *o = malloc(4 * cap); if (!o) return NULL;   // per return: access chain 5, load 4, extract 5, multiply 5, insert 6, store 3
    memcpy(o, code, 20); size_t w = 5; int dec = 0, decl = 0, in = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (!dec && ((op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == OpVariable)) {   // the SpecId decoration, after the other annotations
            o[w++] = 4u << 16 | OpDecorate; o[w++] = spec; o[w++] = 1 /* SpecId */; o[w++] = SPV_FLIP_Y_SPEC_ID; dec = 1; }
        if (!decl && op == 54) {   // the new types / constants, before the first function
            if (add_i32) { o[w++] = 4u << 16 | OpTypeInt; o[w++] = i32; o[w++] = 32; o[w++] = 1; }
            if (add_c) { o[w++] = 4u << 16 | OpConstant; o[w++] = i32; o[w++] = cmem; o[w++] = mem; }
            if (add_p) { o[w++] = 4u << 16 | OpTypePointer; o[w++] = pv4; o[w++] = SC_Output; o[w++] = v4; }
            o[w++] = 4u << 16 | 50 /* OpSpecConstant */; o[w++] = f32; o[w++] = spec; o[w++] = 0x3f800000u; decl = 1; }
        if (op == 54) in = x[2] == entry;
        if (op == 253 && in) {
            uint32_t ptr = posvar;
            if (blk) { ptr = nb++; o[w++] = 5u << 16 | OpAccessChain; o[w++] = pv4; o[w++] = ptr; o[w++] = posvar; o[w++] = cmem; }
            uint32_t ld = nb++, y = nb++, ny = nb++, nv = nb++;
            o[w++] = 4u << 16 | 61 /* OpLoad */; o[w++] = v4; o[w++] = ld; o[w++] = ptr;
            o[w++] = 5u << 16 | 81 /* OpCompositeExtract */; o[w++] = f32; o[w++] = y; o[w++] = ld; o[w++] = 1;
            o[w++] = 5u << 16 | 133 /* OpFMul */; o[w++] = f32; o[w++] = ny; o[w++] = y; o[w++] = spec;
            o[w++] = 6u << 16 | 82 /* OpCompositeInsert */; o[w++] = v4; o[w++] = nv; o[w++] = ny; o[w++] = ld; o[w++] = 1;
            o[w++] = 3u << 16 | 62 /* OpStore */; o[w++] = ptr; o[w++] = nv;
        }
        memcpy(o + w, x, 4 * n); w += n; i += n;
    }
    if (!dec || !decl) { free(o); return NULL; }
    o[3] = nb; *out_words = w; return o;
}

// ---- R11G11B10F's RGBA16F stand-in (DM_VK_B10_AS_RGBA16, nvn_vk.c): what a shader writes into it made what the packed format stores ----
// R11G11B10F's channels are unsigned floats with the half's exponent and 6 (R, G) or 5 (B) mantissa bits. A value stored into one follows Vulkan's rules for them,
// as dm/gfx/tex/b10_conv.h's b10_float_to_chan does for the stand-in's clears and copies: negative values, -0 and -inf become 0, a NaN stays a NaN, +inf stays +inf,
// finite values round to the nearest (ties to even), and those past the largest finite value (65024 for R / G, 64512 for B) become it. RGBA16F keeps a negative or
// out-of-range value and rounds to 10 mantissa bits, so a later pass would read what the Switch never stores (a negative value through log2 / pow gives NaN).
// bq_emit writes the rule for a 32-bit float scalar or vector in integer arithmetic on the float's bits, which no rounding mode or fast-math folding of a driver's
// can change: components 0 and 1 by R / G's rule, 2 by B's, 3 by R's (the callers keep the shader's alpha there, or write 1).
enum { BQ_23, BQ_31, BQ_255, BQ_MANT, BQ_ONE23, BQ_112, BQ_113, BQ_0, BQ_1, BQ_D, BQ_DMASK, BQ_DHALF, BQ_MAXB, BQ_D113, BQ_F0, BQ_SCALE, BQ_N };
enum { BQ_PRELUDE_ROOM = 1500, BQ_EMIT_ROOM = 64 * 6 };   // words bq_prelude and one bq_emit write at most
typedef struct {
    uint32_t tf, tu, tb, vf[5], vu[5], vb[5];   // the module's 32-bit float, 32-bit unsigned and bool types and their vectors (0: none yet)
    int need[5];                                 // component counts the patch works on (1: scalar)
    uint32_t c[5][BQ_N], f1;                     // per component count, the constants (bq_prelude); f1: the float 1.0
} b10q;
static uint32_t bq_val(int k, uint32_t comp) {   // constant k's value for component comp
    int b = comp == 2; uint32_t d = b ? 18 : 17;  // (the float's mantissa bits below the packed channel's: 23 - 5 for B, 23 - 6 for R / G)
    switch (k) {
        case BQ_23: return 23; case BQ_31: return 31; case BQ_255: return 255; case BQ_MANT: return 0x7fffffu; case BQ_ONE23: return 0x800000u;
        case BQ_112: return 112; case BQ_113: return 113; case BQ_0: return 0; case BQ_1: return 1;
        case BQ_D: return d; case BQ_DMASK: return (1u << d) - 1; case BQ_DHALF: return 1u << (d - 1); case BQ_D113: return d + 113;
        case BQ_MAXB: return b ? 0x477C0000u : 0x477E0000u;      // 64512.0f, 65024.0f
        case BQ_F0: return 0; case BQ_SCALE: return b ? 0x36000000u : 0x35800000u;   // 2^-19, 2^-20: the channel's smallest denormal
    }
    return 0;
}
static void bq_type(b10q *q, const uint32_t *x, uint32_t op, uint32_t n) {   // every type declaration, in order
    if (op == OpTypeFloat && n >= 3 && x[2] == 32) q->tf = x[1];
    else if (op == OpTypeInt && n >= 4 && x[2] == 32 && x[3] == 0) q->tu = x[1];
    else if (op == 20 /* OpTypeBool */ && n == 2) q->tb = x[1];
    else if (op == OpTypeVector && n == 4 && x[3] >= 2 && x[3] <= 4) {
        if (q->tf && x[2] == q->tf) q->vf[x[3]] = x[1]; else if (q->tu && x[2] == q->tu) q->vu[x[3]] = x[1]; else if (q->tb && x[2] == q->tb) q->vb[x[3]] = x[1]; }
}
// before the first function: the types still missing, then each needed count's constants (and the float 1.0)
static size_t bq_prelude(b10q *q, uint32_t *o, size_t w, uint32_t *next) {
    if (!q->tu) { q->tu = (*next)++; o[w++] = 4u << 16 | OpTypeInt; o[w++] = q->tu; o[w++] = 32; o[w++] = 0; }
    if (!q->tb) { q->tb = (*next)++; o[w++] = 2u << 16 | 20; o[w++] = q->tb; }
    for (uint32_t k = 2; k <= 4; k++) if (q->need[k]) {
        if (!q->vf[k]) { q->vf[k] = (*next)++; o[w++] = 4u << 16 | OpTypeVector; o[w++] = q->vf[k]; o[w++] = q->tf; o[w++] = k; }
        if (!q->vu[k]) { q->vu[k] = (*next)++; o[w++] = 4u << 16 | OpTypeVector; o[w++] = q->vu[k]; o[w++] = q->tu; o[w++] = k; }
        if (!q->vb[k]) { q->vb[k] = (*next)++; o[w++] = 4u << 16 | OpTypeVector; o[w++] = q->vb[k]; o[w++] = q->tb; o[w++] = k; } }
    q->f1 = (*next)++; o[w++] = 4u << 16 | OpConstant; o[w++] = q->tf; o[w++] = q->f1; o[w++] = 0x3f800000u;
    for (uint32_t k = 1; k <= 4; k++) if (q->need[k]) for (int c = 0; c < BQ_N; c++) {
        int fl = c == BQ_F0 || c == BQ_SCALE; uint32_t st = fl ? q->tf : q->tu, comp[4];
        for (uint32_t j = 0; j < k; j++) {
            comp[j] = 0; for (uint32_t i = 0; i < j; i++) if (bq_val(c, i) == bq_val(c, j)) { comp[j] = comp[i]; break; }
            if (!comp[j]) { comp[j] = (*next)++; o[w++] = 4u << 16 | OpConstant; o[w++] = st; o[w++] = comp[j]; o[w++] = bq_val(c, j); } }
        if (k == 1) q->c[1][c] = comp[0];
        else { q->c[k][c] = (*next)++; o[w++] = (3u + k) << 16 | 44 /* OpConstantComposite */; o[w++] = fl ? q->vf[k] : q->vu[k]; o[w++] = q->c[k][c];
               for (uint32_t j = 0; j < k; j++) o[w++] = comp[j]; } }
    return w;
}
static uint32_t bq_op(uint32_t *o, size_t *w, uint32_t *next, uint32_t op, uint32_t t, int na, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = (*next)++; o[(*w)++] = (uint32_t)(3 + na) << 16 | op; o[(*w)++] = t; o[(*w)++] = r; o[(*w)++] = a; if (na > 1) o[(*w)++] = b; if (na > 2) o[(*w)++] = c;
    return r;
}
// value v (a 32-bit float scalar, k 1, or vector of k) as the packed format stores it; returns the new value's id
static uint32_t bq_emit(const b10q *q, uint32_t k, uint32_t v, uint32_t *o, size_t *w, uint32_t *next) {
    enum { Shr = 194, Shl = 196, Or = 197, And = 199, IAdd = 128, ISub = 130, IEq = 170, INe = 171, UGt = 172, UGe = 174, LOr = 166, LAnd = 167, LNot = 168, Sel = 169,
           Bitcast = 124, UToF = 112, FMul = 133 };
    const uint32_t *C = q->c[k]; uint32_t TF = k == 1 ? q->tf : q->vf[k], TU = k == 1 ? q->tu : q->vu[k], TB = k == 1 ? q->tb : q->vb[k];
    #define O1(op, t, a) bq_op(o, w, next, op, t, 1, a, 0, 0)
    #define O2(op, t, a, b) bq_op(o, w, next, op, t, 2, a, b, 0)
    #define O3(op, t, a, b, c) bq_op(o, w, next, op, t, 3, a, b, c)
    uint32_t u = O1(Bitcast, TU, v);
    uint32_t t = O2(Shr, TU, u, C[BQ_23]), e8 = O2(And, TU, t, C[BQ_255]);
    uint32_t sg = O2(Shr, TU, u, C[BQ_31]), neg = O2(INe, TB, sg, C[BQ_0]);
    uint32_t spec = O2(IEq, TB, e8, C[BQ_255]);                                     // infinity or NaN
    uint32_t mn = O2(And, TU, u, C[BQ_MANT]), mnz = O2(INe, TB, mn, C[BQ_0]), nan = O2(LAnd, TB, spec, mnz);
    uint32_t gt = O2(UGt, TB, u, C[BQ_MAXB]), uc = O3(Sel, TU, gt, C[BQ_MAXB], u);   // (a positive finite value: at most the largest finite one; else unused)
    uint32_t ec = O2(Shr, TU, uc, C[BQ_23]);
    // the packed format's normal range (2^-14 and up): the float's bits rounded at bit d, ties to even (a carry raises the exponent)
    uint32_t r = O2(Shr, TU, uc, C[BQ_D]), rem = O2(And, TU, uc, C[BQ_DMASK]);
    uint32_t g1 = O2(UGt, TB, rem, C[BQ_DHALF]), eh = O2(IEq, TB, rem, C[BQ_DHALF]), ro = O2(And, TU, r, C[BQ_1]), odd = O2(INe, TB, ro, C[BQ_0]);
    uint32_t tie = O2(LAnd, TB, eh, odd), up = O2(LOr, TB, g1, tie), inc = O3(Sel, TU, up, C[BQ_1], C[BQ_0]), r2 = O2(IAdd, TU, r, inc);
    uint32_t nb = O2(Shl, TU, r2, C[BQ_D]), nf = O1(Bitcast, TF, nb);
    // below it (the packed format's denormals, quanta of 2^-20 / 2^-19): the mantissa with its leading one, shifted down by d + 113 - exponent (18..31), rounded
    uint32_t ma = O2(And, TU, uc, C[BQ_MANT]), mant = O2(Or, TU, ma, C[BQ_ONE23]);
    uint32_t egt = O2(UGt, TB, ec, C[BQ_112]), ecl = O3(Sel, TU, egt, C[BQ_112], ec);
    uint32_t sh0 = O2(ISub, TU, C[BQ_D113], ecl), sgt = O2(UGt, TB, sh0, C[BQ_31]), sh = O3(Sel, TU, sgt, C[BQ_31], sh0);
    uint32_t qd = O2(Shr, TU, mant, sh), m1 = O2(Shl, TU, C[BQ_1], sh), msk = O2(ISub, TU, m1, C[BQ_1]), rem2 = O2(And, TU, mant, msk);
    uint32_t shm = O2(ISub, TU, sh, C[BQ_1]), h2 = O2(Shl, TU, C[BQ_1], shm);
    uint32_t g2 = O2(UGt, TB, rem2, h2), e2 = O2(IEq, TB, rem2, h2), qo = O2(And, TU, qd, C[BQ_1]), odd2 = O2(INe, TB, qo, C[BQ_0]);
    uint32_t tie2 = O2(LAnd, TB, e2, odd2), up2 = O2(LOr, TB, g2, tie2), inc2 = O3(Sel, TU, up2, C[BQ_1], C[BQ_0]), q2 = O2(IAdd, TU, qd, inc2);
    uint32_t qf = O1(UToF, TF, q2), df = O2(FMul, TF, qf, C[BQ_SCALE]);
    uint32_t norm = O2(UGe, TB, ec, C[BQ_113]), fin = O3(Sel, TF, norm, nf, df);
    uint32_t keep = O3(Sel, TF, spec, v, fin);                                      // NaN and +inf as they are
    uint32_t nn = O1(LNot, TB, nan), z = O2(LAnd, TB, neg, nn);
    return O3(Sel, TF, z, C[BQ_F0], keep);                                           // negative, -0 and -inf: 0
    #undef O1
    #undef O2
    #undef O3
}
// A copy of a fragment shader whose float outputs at the locations in mask (scalar or vector, not BuiltIn, not a dual-source blend's second output) are made what
//   R11G11B10F stores, components 0-2 (the alpha as the shader wrote it): before every return of the entry point each is loaded, rounded and stored again, so
//   whatever the shader did with it before (component stores, reads back) stays as it was. For the colour targets DM_VK_B10_AS_RGBA16 draws into its RGBA16F
//   stand-in without blending (nvn_vk.c pso_make). NULL if no output there can be done
uint32_t *spv_b10_outputs(const uint32_t *code, size_t words, uint32_t mask, size_t *out_words) {
    if (!(mask & 0xff) || words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    int32_t *loc = malloc(4 * (size_t)bound); uint8_t *bi = calloc(bound, 1), *ix1 = calloc(bound, 1);
    if (!loc || !bi || !ix1) { free(loc); free(bi); free(ix1); return NULL; }
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    b10q q; memset(&q, 0, sizeof q); uint32_t entry = 0; int frag = 0; size_t first_fn = 0, nret = 0;
    uint32_t var[8], vty[8], vk[8]; int nv = 0;
    for (size_t i = 5, in = 0; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) goto none; const uint32_t *x = code + i;
        if (op == OpEntryPoint && n >= 3 && !entry) { entry = x[2]; frag = x[1] == 4; }
        else if (op == OpDecorate && n >= 3 && x[1] < bound) { if (x[2] == DecLocation && n >= 4) loc[x[1]] = (int32_t)x[3]; else if (x[2] == DecBuiltIn) bi[x[1]] = 1; else if (x[2] == 32 /* Index */ && n >= 4 && x[3] == 1) ix1[x[1]] = 1; }
        else if (op >= 19 && op <= 39) bq_type(&q, x, op, n);
        else if (op == 54 /* OpFunction */) { if (!first_fn) first_fn = i; in = n >= 3 && x[2] == entry; }
        else if (op == 253 /* OpReturn */ && in) nret++;
        i += n; }
    if (!frag || !first_fn || !nret || !q.tf) goto none;
    for (size_t i = 5; i < first_fn;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[3] == SC_Output && x[2] < bound && loc[x[2]] >= 0 && loc[x[2]] < 8 && (mask >> loc[x[2]] & 1) && !bi[x[2]] && !ix1[x[2]] && nv < 8) {
            const uint32_t *p = def_of(code, words, x[1]); uint32_t T = p && (p[0] & 0xffff) == OpTypePointer ? p[3] : 0, k = 0;
            if (T && T == q.tf) k = 1;
            else for (uint32_t c = 2; c <= 4; c++) if (T && q.vf[c] == T) k = c;
            if (k) { var[nv] = x[2]; vty[nv] = T; vk[nv] = k; q.need[k] = 1; nv++; } }
        i += n; }
    if (!nv) goto none;
    size_t cap = words + BQ_PRELUDE_ROOM + nret * (size_t)nv * (BQ_EMIT_ROOM + 24); uint32_t *o = malloc(4 * cap); if (!o) goto none;
    uint32_t next = bound; size_t w = 5; memcpy(o, code, 20);
    for (size_t i = 5, in = 0; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (i == first_fn) w = bq_prelude(&q, o, w, &next);
        if (op == 54) in = n >= 3 && x[2] == entry;
        if (op == 253 && in) for (int v = 0; v < nv; v++) {
            uint32_t ld = next++; o[w++] = 4u << 16 | 61 /* OpLoad */; o[w++] = vty[v]; o[w++] = ld; o[w++] = var[v];
            uint32_t qv = bq_emit(&q, vk[v], ld, o, &w, &next);
            if (vk[v] == 4) { uint32_t s = next++; o[w++] = 9u << 16 | 79 /* OpVectorShuffle */; o[w++] = vty[v]; o[w++] = s; o[w++] = qv; o[w++] = ld; o[w++] = 0; o[w++] = 1; o[w++] = 2; o[w++] = 7; qv = s; }
            o[w++] = 3u << 16 | 62 /* OpStore */; o[w++] = var[v]; o[w++] = qv; }
        memcpy(o + w, x, 4 * n); w += n; i += n; }
    free(loc); free(bi); free(ix1);
    o[3] = next; *out_words = w; return o;
none:
    free(loc); free(bi); free(ix1); return NULL;
}
// A copy in which every OpImageWrite through a storage image the shader declares R11fG11fB10f (not a texel buffer) writes its texel as R11G11B10F stores it, alpha 1
//   (the packed format has none: read back, it is 1). With DM_VK_B10_AS_RGBA16 such an image is the RGBA16F stand-in, which would keep what the packed format cannot
//   hold (applied before spv_image_format_swap re-declares it Rgba16f). A 3-component texel becomes a 4-component one (Rgba16f takes 4). *n_left: writes of fewer
//   components, left as they are. NULL if there is no write to patch
uint32_t *spv_b10_image_writes(const uint32_t *code, size_t words, size_t *out_words, int *n_left) {
    if (n_left) *n_left = 0;
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint8_t *ty = calloc(bound, 1); uint32_t *rt = calloc(bound, 4);   // ty: 1 a type, 2 an R11G11B10F storage image type; rt: a value's type
    if (!ty || !rt) { free(ty); free(rt); return NULL; }
    b10q q; memset(&q, 0, sizeof q); size_t first_fn = 0, nw = 0; int left = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) goto none; const uint32_t *x = code + i;
        if (op >= 19 && op <= 39) { if (n >= 2 && x[1] < bound) { ty[x[1]] = 1; bq_type(&q, x, op, n);
                if (op == OpTypeImage && n >= 9 && x[7] == 2 && x[8] == SPV_IMGFMT_R11G11B10F && x[3] != SPV_DIM_BUFFER) ty[x[1]] = 2; } }
        else if (n >= 3 && x[1] < bound && ty[x[1]] && x[2] < bound) rt[x[2]] = x[1];   // (an instruction with a result type: its result; annotations come before any type)
        if (op == 54 && !first_fn) first_fn = i;
        i += n; }
    for (size_t i = first_fn ? first_fn : words; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == 99 /* OpImageWrite */ && n >= 4 && x[1] < bound && x[3] < bound && rt[x[1]] < bound && ty[rt[x[1]]] == 2) {
            uint32_t T = rt[x[3]], k = 0; for (uint32_t c = 3; c <= 4; c++) if (T && q.vf[c] == T) k = c;
            if (k) { q.need[k] = 1; q.need[4] = 1; nw++; } else left++; }
        i += n; }
    if (n_left) *n_left = left;
    if (!nw || !q.tf) goto none;
    uint32_t *o = malloc(4 * (words + BQ_PRELUDE_ROOM + nw * (BQ_EMIT_ROOM + 16))); if (!o) goto none;
    uint32_t next = bound; size_t w = 5; memcpy(o, code, 20);
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (i == first_fn) w = bq_prelude(&q, o, w, &next);
        uint32_t T = op == 99 && n >= 4 && x[1] < bound && x[3] < bound && rt[x[1]] < bound && ty[rt[x[1]]] == 2 ? rt[x[3]] : 0, k = 0;
        for (uint32_t c = 3; c <= 4; c++) if (T && q.vf[c] == T) k = c;
        if (k) {
            uint32_t qv = bq_emit(&q, k, x[3], o, &w, &next), t4 = next++;
            if (k == 4) { o[w++] = 6u << 16 | 82 /* OpCompositeInsert */; o[w++] = T; o[w++] = t4; o[w++] = q.f1; o[w++] = qv; o[w++] = 3; }
            else { o[w++] = 5u << 16 | 80 /* OpCompositeConstruct */; o[w++] = q.vf[4]; o[w++] = t4; o[w++] = qv; o[w++] = q.f1; }
            memcpy(o + w, x, 4 * n); o[w + 3] = t4; w += n;
        } else { memcpy(o + w, x, 4 * n); w += n; }
        i += n; }
    free(ty); free(rt);
    o[3] = next; *out_words = w; return o;
none:
    free(ty); free(rt); return NULL;
}

// ---- vertex inputs fetched as integers (the Mali white screen fix) ----
// A copy of a vertex shader whose float inputs at the locations in umask / smask are declared uint / int vectors of the same component count and OpBitcast to
// float at each load: the pipeline keeps the attribute's 8 / 16-bit UINT / SINT format. The Switch's GPU hands a float input fed by an integer format the integer's
// bits (the shader converts them itself); Vulkan leaves that pairing undefined (VUID-VkGraphicsPipelineCreateInfo-Input-08733), and Arm's drivers convert the
// integer to a float first (255 -> 255.0f), so the shader's own conversion read 0x437f0000: Sword / Shield's lit field went white on Mali.
// hmask (half floats read as uint and converted with UnpackHalf2x16) is a diagnostic left in. Loads through an access chain to one component and whole-vector
// loads are handled; any other use of such an input gives NULL (the caller keeps the shader's own module)
uint32_t *spv_vin_raw(const uint32_t *code, size_t words, uint32_t hmask, uint32_t umask, uint32_t smask, size_t *out_words) {
    enum { OpExtInstImport_ = 11, OpExtInst_ = 12, OpFunction_ = 54, OpLoad_ = 61, OpAccessChain_ = 65, OpInBoundsAccessChain_ = 66, OpCompositeConstruct_ = 80,
           OpCompositeExtract_ = 81, OpBitcast_ = 124, UnpackHalf2x16 = 62 };
    uint32_t mask = hmask | umask | smask;
    if (words < 5 || code[0] != 0x07230203u || !mask) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint32_t *locof = calloc(bound, 4), *ptee = calloc(bound, 4); uint8_t *fw = calloc(bound, 1), *tgt = calloc(bound, 1), *rawp = calloc(bound, 1), *mode = calloc(bound, 1);
    // fw: 1 the 32-bit float, 2-4 its vectors; tgt: a target input variable (its component count); rawp: an access chain into one (a component pointer);
    // mode: 0 half, 1 uint bits, 2 int bits (of a target variable / component pointer)
    uint32_t glsl = 0, tf = 0, tu = 0, ti = 0, uv[5] = { 0 }, iv[5] = { 0 }, v2f = 0, pin_u = 0, pin_i = 0, pin_uv[5] = { 0 }, pin_iv[5] = { 0 };
    size_t first_var = 0, first_fn = 0, ntgt = 0, nload = 0; int bad = 0, need[5] = { 0 }, needi[5] = { 0 }, needh = 0, needs_i = 0;
    if (!locof || !ptee || !fw || !tgt || !rawp || !mode) goto none;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) goto none; const uint32_t *x = code + i;
        if (op == OpExtInstImport_ && n >= 3 && !strncmp((const char *)(x + 2), "GLSL.std.450", 4 * (n - 2))) glsl = x[1];
        else if (op == OpDecorate && n >= 4 && x[2] == DecLocation && x[1] < bound) locof[x[1]] = x[3] + 1;
        else if (op == OpTypeFloat && n >= 3 && x[2] == 32 && x[1] < bound) { tf = x[1]; fw[x[1]] = 1; }
        else if (op == OpTypeInt && n >= 4 && x[2] == 32 && x[3] == 0 && !first_fn) tu = x[1];
        else if (op == OpTypeInt && n >= 4 && x[2] == 32 && x[3] == 1 && !first_fn) ti = x[1];
        else if (op == OpTypeVector && n == 4 && x[1] < bound && x[3] >= 2 && x[3] <= 4) { if (tf && x[2] == tf) { fw[x[1]] = (uint8_t)x[3]; if (x[3] == 2) v2f = x[1]; }
            else if (tu && x[2] == tu) uv[x[3]] = x[1]; else if (ti && x[2] == ti) iv[x[3]] = x[1]; }
        else if (op == OpTypePointer && n == 4 && x[1] < bound && x[2] == SC_Input) { ptee[x[1]] = x[3];
            if (tu && x[3] == tu) pin_u = x[1];
            if (ti && x[3] == ti) pin_i = x[1];
            for (int k = 2; k <= 4; k++) { if (uv[k] && x[3] == uv[k]) pin_uv[k] = x[1]; if (iv[k] && x[3] == iv[k]) pin_iv[k] = x[1]; } }
        else if (op == OpVariable && n >= 4 && !first_fn) {
            if (x[3] == SC_Input && x[2] < bound && x[1] < bound && locof[x[2]] && locof[x[2]] <= 32 && (mask >> (locof[x[2]] - 1) & 1) && ptee[x[1]] < bound && fw[ptee[x[1]]]) {
                uint32_t lb = 1u << (locof[x[2]] - 1); int k = fw[ptee[x[1]]], md = hmask & lb ? 0 : umask & lb ? 1 : 2;
                tgt[x[2]] = (uint8_t)k; mode[x[2]] = (uint8_t)md;
                if (md == 2) { needi[k] = 1; needs_i = 1; } else need[k] = 1;
                if (md == 0) needh = 1;
                ntgt++; first_var = 1; } }
        else if (op == OpFunction_ && !first_fn) first_fn = i;
        else if ((op == OpAccessChain_ || op == OpInBoundsAccessChain_) && n == 5 && x[2] < bound && x[3] < bound && tgt[x[3]]) { rawp[x[2]] = 1; mode[x[2]] = mode[x[3]]; }
        else if ((op == OpAccessChain_ || op == OpInBoundsAccessChain_) && x[3] < bound && tgt[x[3]]) bad = 1;   // (deeper chains: not handled)
        else if (op == OpLoad_ && n >= 4 && x[3] < bound && (rawp[x[3]] || tgt[x[3]])) nload++;
        i += n; }
    if (!ntgt || (needh && !glsl) || !tf || !first_var || !first_fn || bad) goto none;
    // any other reference to a target variable or a component pointer (besides its declaration, the chains, the loads, the entry point's interface) → not handled
    for (size_t i = 5; i < words; i += code[i] >> 16) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        // (only the instructions that can take a pointer operand: a literal elsewhere may equal an id)
        if (op != 62 /* OpStore */ && op != 63 /* OpCopyMemory */ && op != 83 /* OpCopyObject */ && op != 57 /* OpFunctionCall */ && op != 67 /* OpPtrAccessChain */
            && op != 169 /* OpSelect */ && op != 245 /* OpPhi */) continue;
        for (uint32_t j = 1; j < n; j++) if (x[j] < bound && (tgt[x[j]] || rawp[x[j]])) { bad = 1; break; } }
    if (bad) goto none;
    {
    uint32_t next = bound; size_t cap = words + 160 + nload * 80; uint32_t *o = malloc(4 * cap); if (!o) goto none; size_t w = 5; memcpy(o, code, 20);
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (i == first_fn) {   // the types the copy needs, then the inputs it retypes (moved here: their new pointer types may only exist now), before the first function
            if (!tu) { tu = next++; o[w++] = 4u << 16 | OpTypeInt; o[w++] = tu; o[w++] = 32; o[w++] = 0; }
            if (needs_i && !ti) { ti = next++; o[w++] = 4u << 16 | OpTypeInt; o[w++] = ti; o[w++] = 32; o[w++] = 1; }
            if (needh && !v2f) { v2f = next++; o[w++] = 4u << 16 | OpTypeVector; o[w++] = v2f; o[w++] = tf; o[w++] = 2; }
            if (!pin_u) { pin_u = next++; o[w++] = 4u << 16 | OpTypePointer; o[w++] = pin_u; o[w++] = SC_Input; o[w++] = tu; }
            if (needs_i && !pin_i) { pin_i = next++; o[w++] = 4u << 16 | OpTypePointer; o[w++] = pin_i; o[w++] = SC_Input; o[w++] = ti; }
            for (int k = 2; k <= 4; k++) {
                if (need[k]) { if (!uv[k]) { uv[k] = next++; o[w++] = 4u << 16 | OpTypeVector; o[w++] = uv[k]; o[w++] = tu; o[w++] = (uint32_t)k; }
                    if (!pin_uv[k]) { pin_uv[k] = next++; o[w++] = 4u << 16 | OpTypePointer; o[w++] = pin_uv[k]; o[w++] = SC_Input; o[w++] = uv[k]; } }
                if (needi[k]) { if (!iv[k]) { iv[k] = next++; o[w++] = 4u << 16 | OpTypeVector; o[w++] = iv[k]; o[w++] = ti; o[w++] = (uint32_t)k; }
                    if (!pin_iv[k]) { pin_iv[k] = next++; o[w++] = 4u << 16 | OpTypePointer; o[w++] = pin_iv[k]; o[w++] = SC_Input; o[w++] = iv[k]; } } }
            for (uint32_t v = 0; v < bound; v++) if (tgt[v]) { if (w + 8 > cap) { cap *= 2; uint32_t *o2 = realloc(o, 4 * cap); if (!o2) { free(o); goto none; } o = o2; }
                int si = mode[v] == 2;
                o[w++] = 4u << 16 | OpVariable; o[w++] = tgt[v] == 1 ? (si ? pin_i : pin_u) : (si ? pin_iv[tgt[v]] : pin_uv[tgt[v]]); o[w++] = v; o[w++] = SC_Input; }
        }
        memcpy(o + w, x, 4 * n);
        if (op == OpVariable && x[2] < bound && tgt[x[2]]) { }   // (declared again before the first function)
        else if ((op == OpAccessChain_ || op == OpInBoundsAccessChain_) && x[2] < bound && rawp[x[2]]) { o[w + 1] = mode[x[2]] == 2 ? pin_i : pin_u; w += n; }
        else if (op == OpLoad_ && n >= 4 && x[3] < bound && (rawp[x[3]] || tgt[x[3]])) {
            uint32_t res = x[2]; int k = rawp[x[3]] ? 1 : tgt[x[3]], md = mode[x[3]];
            uint32_t ity = md == 2 ? (k == 1 ? ti : iv[k]) : (k == 1 ? tu : uv[k]);
            uint32_t u = next++; o[w + 1] = ity; o[w + 2] = u; w += n;
            if (md != 0) { o[w++] = 4u << 16 | OpBitcast_; o[w++] = x[1]; o[w++] = res; o[w++] = u; }   // (same component count and width: one bitcast)
            else {
                uint32_t f[4];
                for (int c = 0; c < k; c++) {
                    uint32_t uc = u;
                    if (k > 1) { uc = next++; o[w++] = 5u << 16 | OpCompositeExtract_; o[w++] = tu; o[w++] = uc; o[w++] = u; o[w++] = (uint32_t)c; }
                    uint32_t h = next++; o[w++] = 6u << 16 | OpExtInst_; o[w++] = v2f; o[w++] = h; o[w++] = glsl; o[w++] = UnpackHalf2x16; o[w++] = uc;
                    f[c] = k == 1 ? res : next++; o[w++] = 5u << 16 | OpCompositeExtract_; o[w++] = tf; o[w++] = f[c]; o[w++] = h; o[w++] = 0;
                }
                if (k > 1) { o[w++] = (3u + (uint32_t)k) << 16 | OpCompositeConstruct_; o[w++] = x[1]; o[w++] = res; for (int c = 0; c < k; c++) o[w++] = f[c]; }
            }
        }
        else w += n;
        i += n;
        if (w + 128 > cap) { cap *= 2; uint32_t *o2 = realloc(o, 4 * cap); if (!o2) { free(o); goto none; } o = o2; }
    }
    o[3] = next; *out_words = w;
    free(locof); free(ptee); free(fw); free(tgt); free(rawp); free(mode); return o;
    }
  none:
    free(locof); free(ptee); free(fw); free(tgt); free(rawp); free(mode); return NULL;
}
