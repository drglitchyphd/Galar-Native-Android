// Golden-frame recordings, the Vulkan replay's side (golden_rec.h's golden_be_*): #included at the end of nvn_vk.c, which it reads directly (its tables, its replay
// state, its images). The recorder (golden_rec.c) and the file format (golden_fmt.h) know nothing of Vulkan; what this writes is NVN-level (registrations, records,
// memory) except an image's texels, which carry the VkFormat they were read in (golden_texi).
#include "golden_rec.h"

// ---------- Tables ----------
int golden_be_pools(golden_be_pool_t *out, int max) {
    pthread_mutex_lock(&g_mu); int n = g_npool;
    for (int i = 0; i < n && i < max; i++) out[i] = (golden_be_pool_t){ (uint64_t)g_pools[i].lo, (uint64_t)(g_pools[i].hi - g_pools[i].lo) };
    pthread_mutex_unlock(&g_mu); return n;
}
int golden_be_pool_of(uint64_t addr, uint64_t *lo, uint64_t *hi) {
    pthread_mutex_lock(&g_mu); int i = pool_find_linear(addr, 1);
    if (i >= 0) { *lo = (uint64_t)g_pools[i].base; *hi = (uint64_t)(g_pools[i].base + g_pools[i].size); }
    pthread_mutex_unlock(&g_mu); return i >= 0;
}
int golden_be_treg_n(void) { pthread_mutex_lock(&g_rmu); int n = g_ntreg; pthread_mutex_unlock(&g_rmu); return n; }
int golden_be_treg_get(int id, tex_t *t, view_t *v, int *has_v) {
    pthread_mutex_lock(&g_rmu); int ok = id >= 0 && id < g_ntreg && g_treg[id].ok;
    if (ok) { *t = g_treg[id].t; *v = g_treg[id].v; *has_v = g_treg[id].has_v; }
    pthread_mutex_unlock(&g_rmu); return ok;
}
int golden_be_sreg_n(void) { pthread_mutex_lock(&g_rmu); int n = g_nsreg; pthread_mutex_unlock(&g_rmu); return n; }
int golden_be_sreg_get(int id, samp_par *p) {
    pthread_mutex_lock(&g_rmu); int ok = id >= 0 && id < g_nsreg && g_sreg_ok[id]; if (ok) *p = g_sreg[id]; pthread_mutex_unlock(&g_rmu); return ok;
}
void golden_be_globals(int *origin_ll, int *depth_zero, int *tex_pool_n, char *gpu, size_t ngpu) {
    *origin_ll = atomic_load(&g_origin_ll); *depth_zero = atomic_load(&g_depth_zero); *tex_pool_n = g_tpool_n;
    snprintf(gpu, ngpu, "%s (vendor %#x, driver %#x, Vulkan %u.%u)", g_props.deviceName, g_props.vendorID, g_props.driverVersion, VK_API_VERSION_MAJOR(g_props.apiVersion), VK_API_VERSION_MINOR(g_props.apiVersion));
}
void golden_be_finish(void) { mtl_finish(); }

// ---------- The replay state as command records (the frame's prologue: what earlier frames bound and this one relies on) ----------
static uint8_t *gv_op(uint8_t **p, uint8_t *end, uint32_t op, const void *a, size_t n) {
    uint32_t sz = (uint32_t)((8 + n + 7) & ~(size_t)7);
    if (!*p || *p + sz + 8 > end) { *p = NULL; return NULL; }
    op_hdr h = { op, sz }; memcpy(*p, &h, 8); memset(*p + 8, 0, sz - 8); if (n) memcpy(*p + 8, a, n); *p += sz; return *p;
}
size_t golden_be_state_ops(uint8_t *buf, size_t cap) {
    uint8_t *p = buf, *end = buf + cap;
    // programs, per stage, as NVN binds them (bind_stages combines them again)
    for (int st = 0; st < 5; st++) if (S.bst[st]) {
        int bits = 0; for (int k = st; k < 5; k++) if (S.bst[k] == S.bst[st]) bits |= 1 << k;
        int first = 1; for (int k = 0; k < st; k++) if (S.bst[k] == S.bst[st]) first = 0;
        if (first) { p_bind_program b = { S.bst[st], bits }; gv_op(&p, end, OP_BIND_PROGRAM, &b, sizeof b); }
    }
    if (S.cprog) { p_bind_program b = { S.cprog, 0x20 }; gv_op(&p, end, OP_BIND_PROGRAM, &b, sizeof b); }
    for (int i = 0; i < 8; i++) { p_bind_state b = { 0 }; memcpy(b.bytes, &S.blend[i], sizeof S.blend[i]); gv_op(&p, end, OP_BIND_STATE, &b, sizeof b); }
    { p_bind_state b = { 1 }; memcpy(b.bytes, &S.color, sizeof S.color); gv_op(&p, end, OP_BIND_STATE, &b, sizeof b); }
    { p_bind_state b = { 2 }; memcpy(b.bytes, &S.chmask, sizeof S.chmask); gv_op(&p, end, OP_BIND_STATE, &b, sizeof b); }
    { p_bind_state b = { 3 }; memcpy(b.bytes, &S.ms, sizeof S.ms); gv_op(&p, end, OP_BIND_STATE, &b, sizeof b); }
    { p_bind_state b = { 4 }; memcpy(b.bytes, &S.poly, sizeof S.poly); gv_op(&p, end, OP_BIND_STATE, &b, sizeof b); }
    { p_bind_state b = { 5 }; memcpy(b.bytes, &S.ds, sizeof S.ds); gv_op(&p, end, OP_BIND_STATE, &b, sizeof b); }
    { p_bind_vattr b = { 0, S.nattr }; memcpy(b.st, S.attr, sizeof b.st); gv_op(&p, end, OP_BIND_VATTR, &b, sizeof b); }
    { p_bind_vstream b = { 0, S.nstream }; memcpy(b.st, S.stream, sizeof b.st); gv_op(&p, end, OP_BIND_VSTREAM, &b, sizeof b); }
    { p_bind_vbuf b = { 0, 16 }; memcpy(b.r, S.vbuf, sizeof b.r); gv_op(&p, end, OP_BIND_VBUF, &b, sizeof b); }
    for (int st = 0; st < 6; st++) for (int i = 0; i < 16; i++) {
        if (S.ubo[st][i].addr || S.ubo[st][i].size) { p_bind_buf b = S.ubo[st][i]; b.stage = st; b.index = i; gv_op(&p, end, OP_BIND_UBO, &b, sizeof b); }
        if (S.ssbo[st][i].addr || S.ssbo[st][i].size) { p_bind_buf b = S.ssbo[st][i]; b.stage = st; b.index = i; gv_op(&p, end, OP_BIND_SSBO, &b, sizeof b); }
    }
    for (int st = 0; st < 6; st++) {
        struct { uint32_t op; const uint64_t *h; int n; } tab[3] = { { OP_BIND_TEX, S.tex[st], NTEX }, { OP_BIND_SAMP, S.samp[st], 128 }, { OP_BIND_IMG, S.img[st], 32 } };
        for (int k = 0; k < 3; k++) for (int f = 0; f < tab[k].n; f += 32) {
            int any = 0; for (int i = f; i < f + 32; i++) any |= tab[k].h[i] != 0;
            if (!any) continue;
            p_bind_handles b; memset(&b, 0, sizeof b); b.stage = st; b.first = f; b.n = 32; memcpy(b.h, tab[k].h + f, sizeof b.h); gv_op(&p, end, tab[k].op, &b, sizeof b);
        }
    }
    for (int f = 0; f < 2; f++) {
        p_stencil a = { 1 << f, (int32_t)S.st_vmask[f], 0, 0, 0 }, b = { 1 << f, 0, (int32_t)S.st_wmask[f], 0, 1 }, c = { 1 << f, 0, 0, (int32_t)S.st_ref[f], 2 };
        gv_op(&p, end, OP_STENCIL, &a, sizeof a); gv_op(&p, end, OP_STENCIL, &b, sizeof b); gv_op(&p, end, OP_STENCIL, &c, sizeof c);
    }
    gv_op(&p, end, OP_POLY_OFFSET, &S.po, sizeof S.po);
    if (S.has_vp) { p_viewports v; memset(&v, 0, sizeof v); v.first = 0; v.n = 16; for (int i = 0; i < 16; i++) { memcpy(v.v[i], S.vp[i], 16); v.v[i][5] = 1; } gv_op(&p, end, OP_VIEWPORTS, &v, sizeof v); }
    { p_depth_range d = { S.dr[0], S.dr[1] }; gv_op(&p, end, OP_DEPTH_RANGE, &d, sizeof d); }
    { p_depth_clamp d = { S.depth_clamp }; gv_op(&p, end, OP_DEPTH_CLAMP, &d, sizeof d); }
    { p_blend_color b; memcpy(b.rgba, S.blend_color, 16); gv_op(&p, end, OP_BLEND_COLOR, &b, sizeof b); }
    { p_copy_stride a = { S.copy_stride, 0 }, b = { S.copy_img_stride, 1 }; gv_op(&p, end, OP_COPY_STRIDE, &a, sizeof a); gv_op(&p, end, OP_COPY_STRIDE, &b, sizeof b); }
    { p_rast_discard r = { S.rast_discard }; gv_op(&p, end, OP_RAST_DISCARD, &r, sizeof r); }
    for (int i = 0; i < 4; i++) if (S.tf[i].addr) { p_tf_bind b = S.tf[i]; b.index = i; gv_op(&p, end, OP_TF_BIND, &b, sizeof b); }
    if (R.has_sc) gv_op(&p, end, OP_SCISSOR, &R.sc, sizeof R.sc);
    if (R.has_rt) gv_op(&p, end, OP_SET_RT, &R.rt, sizeof R.rt);
    // clears asked for and not yet done (they wait for the pass they begin)
    for (int i = 0; i < 8; i++) if (R.clear_c[i]) { p_clear_color c = { i, 0xf, { 0 } }; memcpy(c.rgba, R.cc[i].float32, 16); gv_op(&p, end, OP_CLEAR_COLOR, &c, sizeof c); }
    if (R.clear_d || R.clear_s) { p_clear_ds c = { R.cd, R.clear_d, (int32_t)R.cs, R.clear_s ? 0xff : 0 }; gv_op(&p, end, OP_CLEAR_DS, &c, sizeof c); }
    gv_op(&p, end, OP_END, NULL, 0);
    return p ? (size_t)(p - buf) : 0;
}

// ---------- Shaders: the translated SPIR-V and reflection the replay loads (the pack, else the folders) ----------
int golden_be_shader(const uint8_t key[8], int *stage, void **spv, size_t *nspv, void **vkr, size_t *nvkr, void **gl, size_t *ngl) {
    shmap_load();
    intptr_t st1 = (intptr_t)nvn_hm_get(&g_shmap, key);
    if (!st1) { pthread_mutex_lock(&g_shmu); st1 = (intptr_t)nvn_hm_get(&g_livemap, key); pthread_mutex_unlock(&g_shmu); }
    if (!st1) return 0;
    *stage = (int)st1 - 1;
    uint32_t *code = NULL, *glc = NULL; char *meta = NULL; size_t ns = 0, nr = 0, ng = 0;
    if (!pak_get(key, &code, &ns, &meta, &nr, &glc, &ng)) {
        char hex[17], p[1200]; for (int i = 0; i < 8; i++) sprintf(hex + 2 * i, "%02x", key[i]);
        const char *dirs[2] = { g_spv_dir, g_live_dir };
        for (int d = 0; d < 2 && !code; d++) { if (!*dirs[d]) continue;
            free(meta); free(glc); meta = NULL; glc = NULL; ng = 0;
            snprintf(p, sizeof p, "%s/%s.spv", dirs[d], hex); code = file_read(p, &ns);
            snprintf(p, sizeof p, "%s/%s.vkr", dirs[d], hex); meta = file_read(p, &nr);
            snprintf(p, sizeof p, "%s/%s.gl.spv", dirs[d], hex); glc = file_read(p, &ng); }
    }
    if (!code || !meta) { free(code); free(meta); free(glc); return 0; }
    *spv = code; *nspv = ns; *vkr = meta; *nvkr = nr; *gl = glc; *ngl = glc ? ng : 0;
    return 1;
}

// ---------- Images: the ones that exist when the frame starts hold content from earlier frames ----------
static nvn_hmap g_gv_start = {.klen = sizeof(void *)}, g_gv_read = {.klen = sizeof(void *)};
static void gv_hm_clear(nvn_hmap *m) { uint32_t k = m->klen; free(m->keys); free(m->vals); nvn_hm_init(m, k); }
void golden_be_frame_begin(void) {
    mtl_finish();
    pthread_mutex_lock(&g_smu);
    gv_hm_clear(&g_gv_start); gv_hm_clear(&g_gv_read);
    nvn_hmap *tabs[3] = { &g_by_serial, &g_ghost, &g_alias };
    for (int k = 0; k < 3; k++) for (uint32_t i = 0; i < tabs[k]->cap; i++) { void *x = tabs[k]->vals[i]; if (x) nvn_hm_put(&g_gv_start, &x, (void *)1); }
    pthread_mutex_unlock(&g_smu);
}
void golden_be_frame_end(void) { pthread_mutex_lock(&g_smu); gv_hm_clear(&g_gv_start); gv_hm_clear(&g_gv_read); pthread_mutex_unlock(&g_smu); }
// Bytes per texel of the colour formats images are made in here (0: not read back)
static uint32_t gv_fmt_bpe(VkFormat f) {
    int v = (int)f;
    if (v == 1) return 1; if (v >= 2 && v <= 8) return 2; if (v >= 9 && v <= 15) return 1; if (v >= 16 && v <= 22) return 2; if (v >= 23 && v <= 36) return 3;
    if (v >= 37 && v <= 69) return 4; if (v >= 70 && v <= 76) return 2; if (v >= 77 && v <= 83) return 4; if (v >= 84 && v <= 90) return 6; if (v >= 91 && v <= 97) return 8;
    if (v >= 98 && v <= 100) return 4; if (v >= 101 && v <= 103) return 8; if (v >= 104 && v <= 106) return 12; if (v >= 107 && v <= 109) return 16;
    if (v >= 110 && v <= 112) return 8; if (v >= 113 && v <= 115) return 16; if (v == 122 || v == 123) return 4;
    return 0;
}
static uint32_t gv_depth_bytes(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4; }
typedef struct { uint32_t w, h, n; } gv_lv;   // a level's extent and its layers (3D: slices)
static void gv_level(const vtex *x, uint32_t l, gv_lv *o) {
    o->w = x->w >> l ? x->w >> l : 1; o->h = x->h >> l ? x->h >> l : 1; o->n = x->is3d ? (x->d >> l ? x->d >> l : 1) : x->layers;
}
// The image's texels, all levels and layers, into a host buffer in golden_texi's order (one-off command buffer: ordered after everything submitted)
static void gv_readback(vtex *x, const tex_t *t) {
    if (x->samples > 1) { RED_ONCE(150, "golden: a multisampled image from an earlier frame is not recorded (it starts cleared in the player)"); return; }
    int depth = (x->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, stencil = (x->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    uint32_t bpe = depth || stencil ? 0 : gv_fmt_bpe(x->fmt), db = depth ? gv_depth_bytes(x->fmt) : 0;
    if (!depth && !stencil && !bpe) { RED_ONCE(151, "golden: an image of format %d from an earlier frame is not recorded (format not read back)", x->fmt); return; }
    VkBufferImageCopy rg[2 * 16]; uint32_t nr = 0; VkDeviceSize off = 0, out = 0;
    for (uint32_t l = 0; l < x->levels && l < 16; l++) { gv_lv v; gv_level(x, l, &v); VkDeviceSize px = (VkDeviceSize)v.w * v.h * v.n;
        VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, x->is3d ? 1 : v.n };
        VkExtent3D ext = { v.w, v.h, x->is3d ? v.n : 1 };
        if (!depth && !stencil) { rg[nr++] = (VkBufferImageCopy){ off, 0, 0, sub, {0, 0, 0}, ext }; off += px * bpe; out += px * bpe; continue; }
        if (depth) { sub.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; rg[nr++] = (VkBufferImageCopy){ off, 0, 0, sub, {0, 0, 0}, ext }; off = (off + px * db + 3) & ~(VkDeviceSize)3; out += px * 4; }
        if (stencil) { sub.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; rg[nr++] = (VkBufferImageCopy){ off, 0, 0, sub, {0, 0, 0}, ext }; off = (off + px + 3) & ~(VkDeviceSize)3; out += px; }
    }
    vmalloc_t a; void *m; VkBuffer b = buf_make(off ? off : 16, &a, &m);
    VkCommandBuffer cb = once_begin(); vkCmdCopyImageToBuffer(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, b, nr, rg); once_end(cb);
    uint8_t *data = malloc(out ? out : 1); if (!data) { vkDestroyBuffer(g_dev, b, NULL); vk_free(a); return; }
    if (!depth && !stencil) memcpy(data, m, out);
    else { uint8_t *o = data; uint32_t r = 0;
        for (uint32_t l = 0; l < x->levels && l < 16; l++) { gv_lv v; gv_level(x, l, &v); size_t px = (size_t)v.w * v.h * v.n;
            if (depth) { const uint8_t *s = (const uint8_t *)m + rg[r++].bufferOffset; float *d = (float *)o;
                for (size_t i = 0; i < px; i++) { if (db == 2) { uint16_t u; memcpy(&u, s + 2 * i, 2); d[i] = u / 65535.0f; }
                                                   else if (x->fmt == VK_FORMAT_D32_SFLOAT || x->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) memcpy(&d[i], s + 4 * i, 4);
                                                   else { uint32_t u; memcpy(&u, s + 4 * i, 4); d[i] = (u & 0xffffff) / 16777215.0f; } }
                o += px * 4; }
            if (stencil) { memcpy(o, (const uint8_t *)m + rg[r++].bufferOffset, px); o += px; } } }
    vkDestroyBuffer(g_dev, b, NULL); vk_free(a);
    // (R11G11B10F's RGBA16F stand-in, DM_VK_B10_AS_RGBA16, is recorded as it is: vkfmt RGBA16F, nvnfmt 0x3F; golden_be_tex_fill converts for a replay without the test)
    golden_texi h; memset(&h, 0, sizeof h); h.t = *t; h.vkfmt = x->fmt; h.nvnfmt = x->nvn_fmt; h.w = x->w; h.h = x->h; h.d = x->d; h.levels = x->levels < 16 ? x->levels : 16;
    h.layers = x->layers; h.samples = x->samples; h.is3d = (uint32_t)x->is3d; h.kind = depth || stencil ? 1 : 0; h.has_stencil = (uint32_t)stencil; h.bpe = bpe;
    if (!depth && stencil) h.kind = 2;   // (stencil only: no depth floats)
    golden_note_texi(&h, data, out); free(data);
}
// tex_res's first look at a texture while recording (before its own lookup takes g_smu): what the texture is on this phone, its storage when the CPU fills it,
//   and the image's content when the image existed before the frame (read once per image, before this submission writes it)
static void golden_vk_tex_ref(const tex_t *t) {
    if (!t || !t->serial || t->magic != MAGIC('T', 'E', 'X', 0)) return;
    const pool_t *pl = t->pool; if (pl) golden_mem_ref((uint64_t)(uintptr_t)pl, sizeof *pl);
    int linear = (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) && t->d.target != NVN_TEXTURE_TARGET_BUFFER && pl && !(pl->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS);
    pthread_mutex_lock(&g_smu);
    int cpu = nvn_hm_get(&g_cpu_tex, &t->serial) != NULL, live = nvn_hm_get(&g_tex_live, &t->serial) != NULL;
    int packaged = (t->has_layout || cpu || linear) && t->mem && t->d.target != NVN_TEXTURE_TARGET_BUFFER;
    vtex *x = serial_get(t->serial);
    if (!x && !packaged && t->d.target != NVN_TEXTURE_TARGET_BUFFER) { alias_key ak; memset(&ak, 0, sizeof ak); ak.pool = (uint64_t)(uintptr_t)t->pool; ak.off = t->off; ak.d = t->d; x = nvn_hm_get(&g_alias, &ak); }
    int read = x && !x->pkg && x->img && !x->bview && !packaged && nvn_hm_get(&g_gv_start, &x) && !nvn_hm_get(&g_gv_read, &x);
    if (read) { nvn_hm_put(&g_gv_read, &x, (void *)1); vtex_ref(x); }
    pthread_mutex_unlock(&g_smu);
    golden_note_serial(t->serial, live, cpu);
    if (t->mem && (packaged || linear)) golden_mem_ref((uint64_t)(uintptr_t)t->mem, t->L.size);
    if (t->mem && t->d.target == NVN_TEXTURE_TARGET_BUFFER) { int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); golden_mem_ref((uint64_t)(uintptr_t)t->mem, (uint64_t)t->d.width * (bpb ? bpb : 16)); }
    if (read) { gv_readback(x, t); vtex_unref(x); }
}

// ---------- Player side ----------
int golden_be_shot(const tex_t *t, const int crop[4], const char *path) {
    vtex *x = tex_res(t); if (!x || !x->img) return 0;
    int c[4] = { 0, 0, 0, 0 }; if (crop) memcpy(c, crop, sizeof c);
    shot(x, c[2] && c[3] ? c : NULL, path); return 1;
}
int golden_be_tex_fill(const golden_texi *h, const uint8_t *data, size_t n) {
    vtex *x = tex_res(&h->t);
    if (!x || !x->img) { VLOG("[RED] golden: no image for texture ser=%llu (format %#x)", (unsigned long long)h->t.serial, h->nvnfmt); return 0; }
    int depth = (x->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, stencil = (x->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    if (x->w != h->w || x->h != h->h || x->levels != h->levels || (x->is3d ? x->d != h->d : x->layers != h->layers) || (h->kind == 0) != (!depth && !stencil)) {
        VLOG("[RED] golden: texture ser=%llu is %ux%ux%u, %u levels here, %ux%ux%u, %u levels recorded: not filled", (unsigned long long)h->t.serial, x->w, x->h, x->is3d ? x->d : x->layers, x->levels, h->w, h->h, h->is3d ? h->d : h->layers, h->levels);
        return 0; }
    // R11G11B10F recorded in one form and replayed in the other (DM_VK_B10_AS_RGBA16 on in one run only: the packed format, or its RGBA16F stand-in): converted (b10_conv.h)
    golden_texi hc; uint8_t *conv = NULL;
    if (h->kind == 0 && h->nvnfmt == 0x3F && (int)x->fmt != h->vkfmt && ((x->fmt == VK_FORMAT_R16G16B16A16_SFLOAT && h->vkfmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32) ||
                                                                        (x->fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && h->vkfmt == VK_FORMAT_R16G16B16A16_SFLOAT))) {
        size_t px = 0; for (uint32_t l = 0; l < x->levels && l < 16; l++) { gv_lv v; gv_level(x, l, &v); px += (size_t)v.w * v.h * v.n; }
        int expand = x->fmt == VK_FORMAT_R16G16B16A16_SFLOAT; size_t need = px * (expand ? 4 : 8);
        if (need > n || !(conv = malloc(px * (expand ? 8 : 4) + 1))) { VLOG("[RED] golden: texture ser=%llu: %zu bytes recorded, %zu needed", (unsigned long long)h->t.serial, n, need); return 0; }
        if (expand) b10_rows_expand(data, px * 4, (uint32_t)px, 1, conv, px * 8); else b10_rows_pack(data, px * 8, (uint32_t)px, 1, conv, px * 4);
        hc = *h; hc.vkfmt = (int32_t)x->fmt; hc.bpe = expand ? 8 : 4; h = &hc; data = conv; n = px * hc.bpe;
        static int said; if (!said++) VLOG("golden: an R11G11B10F texture recorded as %s, replayed as %s: converted", expand ? "the packed format" : "its RGBA16F stand-in", expand ? "the stand-in" : "the packed format");
    }
    if (h->kind == 0 && ((int)x->fmt != h->vkfmt && gv_fmt_bpe(x->fmt) != h->bpe)) { VLOG("[RED] golden: texture ser=%llu: format %d here, %d recorded: not filled", (unsigned long long)h->t.serial, x->fmt, h->vkfmt); free(conv); return 0; }
    uint32_t db = depth ? gv_depth_bytes(x->fmt) : 0;
    VkBufferImageCopy rg[32]; uint32_t nr = 0; VkDeviceSize off = 0; size_t in = 0;
    for (uint32_t l = 0; l < x->levels && l < 16; l++) { gv_lv v; gv_level(x, l, &v); VkDeviceSize px = (VkDeviceSize)v.w * v.h * v.n;
        VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, x->is3d ? 1 : v.n }; VkExtent3D ext = { v.w, v.h, x->is3d ? v.n : 1 };
        if (h->kind == 0) { rg[nr++] = (VkBufferImageCopy){ off, 0, 0, sub, {0, 0, 0}, ext }; off += px * h->bpe; in += px * h->bpe; continue; }
        if (h->kind == 1) { if (depth) { sub.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; rg[nr++] = (VkBufferImageCopy){ off, 0, 0, sub, {0, 0, 0}, ext }; off = (off + px * db + 3) & ~(VkDeviceSize)3; } in += px * 4; }
        if (h->has_stencil) { if (stencil) { sub.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; rg[nr++] = (VkBufferImageCopy){ off, 0, 0, sub, {0, 0, 0}, ext }; off = (off + px + 3) & ~(VkDeviceSize)3; } in += px; }
    }
    if (in > n) { VLOG("[RED] golden: texture ser=%llu: %zu bytes recorded, %zu needed", (unsigned long long)h->t.serial, n, in); free(conv); return 0; }
    vmalloc_t a; void *m; VkBuffer b = buf_make(off ? off : 16, &a, &m);
    if (h->kind == 0) { memcpy(m, data, in); free(conv); conv = NULL; }   // (a converted copy: not read after this)
    else { const uint8_t *s = data; uint32_t r = 0;
        for (uint32_t l = 0; l < x->levels && l < 16; l++) { gv_lv v; gv_level(x, l, &v); size_t px = (size_t)v.w * v.h * v.n;
            if (h->kind == 1) { if (depth) { uint8_t *d = (uint8_t *)m + rg[r++].bufferOffset;
                    for (size_t i = 0; i < px; i++) { float f; memcpy(&f, s + 4 * i, 4); if (f < 0) f = 0; if (f > 1 && x->fmt != VK_FORMAT_D32_SFLOAT && x->fmt != VK_FORMAT_D32_SFLOAT_S8_UINT) f = 1;
                        if (db == 2) { uint16_t u = (uint16_t)(f * 65535.0f + 0.5f); memcpy(d + 2 * i, &u, 2); }
                        else if (x->fmt == VK_FORMAT_D32_SFLOAT || x->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) memcpy(d + 4 * i, &f, 4);
                        else { uint32_t u = (uint32_t)(f * 16777215.0f + 0.5f); memcpy(d + 4 * i, &u, 4); } } }
                s += px * 4; }
            if (h->has_stencil) { if (stencil) memcpy((uint8_t *)m + rg[r++].bufferOffset, s, px); s += px; } } }
    VkCommandBuffer cb = once_begin(); vkCmdCopyBufferToImage(cb, b, x->img, VK_IMAGE_LAYOUT_GENERAL, nr, rg); once_end(cb);
    vkDestroyBuffer(g_dev, b, NULL); vk_free(a);
    return 1;
}
