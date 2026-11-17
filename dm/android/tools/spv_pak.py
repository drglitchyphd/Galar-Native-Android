#!/usr/bin/env python3
# Packs the Vulkan shader set (<key>.spv + <key>.vkr, dm/android/tools/vk_shaders.sh) into one file the replay maps (nvn_vk.c, DM_SPV_PAK): 77,000 small files are slow to copy to a phone.
# Layout (little endian): "NXSP" u32 version, u32 count, char chain[16], sorted index, then the data.
# Versions 1/2 have 32-byte entries {key[8], spv_off, spv_len, vkr_off, vkr_len}; versions 3/4 add {gl_spv_off, gl_spv_len}.
# Versions 2/4 store every blob as u32 size + zlib stream (Android ships it inside the APK; nvn_vk.c pak_blob inflates on load).
# Usage: spv_pak.py <dir b-<chain>> <out.pak>  |  spv_pak.py --compress <in.pak> <out.pak>
import os, struct, sys, zlib
if sys.argv[1] == '--compress':
    src, out = sys.argv[2], sys.argv[3]; f = open(src, 'rb'); h = f.read(28)
    assert h[:4] == b'NXSP' and struct.unpack('<I', h[4:8])[0] in (1, 3), 'not an uncompressed shader pack'
    ver = struct.unpack('<I', h[4:8])[0]; n = struct.unpack('<I', h[8:12])[0]; stride = 44 if ver == 3 else 32
    # zlib level 6, on every core: on a real Shield pack level 9 gave the same size (ratio 0.297 both) at a quarter of the speed, on one core (#262: this step took a
    #   good part of the 42-minute "pack the shaders" on a Snapdragon 8+ Gen 1). zlib lets go of the GIL, so threads compress side by side; the blobs are written in
    #   the same order as before, as they come, and the index last (its size is known up front): no 700 MB of compressed blobs held in memory
    from concurrent.futures import ThreadPoolExecutor
    level = int(os.environ.get('DM_PAK_LEVEL', '6')); workers = max(1, min(8, os.cpu_count() or 4))
    idx = f.read(stride * n); off = 28 + stride * n; ents = []; jobs = []
    for i in range(n):
        e = idx[stride * i:stride * (i + 1)]; so, sl, ro, rl = struct.unpack('<QIQI', e[8:32]); row = [e[:8]]
        pairs = [(so, sl), (ro, rl)]
        if ver == 3:
            go, gl = struct.unpack('<QI', e[32:44]); pairs.append((go, gl))
        for o, l in pairs:
            if l: jobs.append((len(ents), len(row), o, l))
            row += [0, 0]
        ents.append(row)
    def comp(b): return struct.pack('<I', len(b)) + zlib.compress(b, level)
    with open(out, 'wb') as g, ThreadPoolExecutor(workers) as ex:
        compressed_ver = 4 if ver == 3 else 2
        g.write(b'NXSP' + struct.pack('<II', compressed_ver, n) + h[12:28]); g.write(b'\0' * (stride * n))
        W = 1024   # blobs read and compressed at a time
        for w0 in range(0, len(jobs), W):
            win = jobs[w0:w0 + W]; raw = []
            for _, _, o, l in win: f.seek(o); raw.append(f.read(l))
            for (ei, k, _, _), z in zip(win, ex.map(comp, raw)):
                ents[ei][k] = off; ents[ei][k + 1] = len(z); g.write(z); off += len(z)
        g.seek(28)
        for row in ents:
            if ver == 3: g.write(row[0] + struct.pack('<QIQIQI', *row[1:]))
            else: g.write(row[0] + struct.pack('<QIQI', *row[1:]))
    print(f'{out}: {n} shaders, {off / 1e6:.0f} MB compressed (from {os.path.getsize(src) / 1e6:.0f} MB)'); sys.exit(0)
d, out = sys.argv[1], sys.argv[2]; chain = os.path.basename(d.rstrip('/'))[2:]
keys = sorted(f[:-4] for f in os.listdir(d) if f.endswith('.spv') and not f.endswith('.gl.spv') and os.path.exists(os.path.join(d, f[:-4] + '.vkr')))
has_gl = any(os.path.exists(os.path.join(d, k + '.gl.spv')) for k in keys)
ver = 3 if has_gl else 1; ent = 44 if has_gl else 32; head = 4 + 4 + 4 + 16; off = head + ent * len(keys); idx = []; blobs = []
for k in keys:
    s = open(os.path.join(d, k + '.spv'), 'rb').read(); r = open(os.path.join(d, k + '.vkr'), 'rb').read()
    so, ro = off, off + len(s); blobs += [s, r]; off += len(s) + len(r)
    glp = os.path.join(d, k + '.gl.spv')
    if has_gl and os.path.exists(glp):
        gl = open(glp, 'rb').read(); go = off; blobs.append(gl); off += len(gl)
        idx.append((bytes.fromhex(k), so, len(s), ro, len(r), go, len(gl)))
    elif has_gl: idx.append((bytes.fromhex(k), so, len(s), ro, len(r), 0, 0))
    else: idx.append((bytes.fromhex(k), so, len(s), ro, len(r)))
with open(out, 'wb') as f:
    f.write(b'NXSP' + struct.pack('<II', ver, len(keys)) + chain.encode().ljust(16, b'\0'))
    for row in idx:
        if has_gl: f.write(row[0] + struct.pack('<QIQIQI', *row[1:]))
        else: f.write(row[0] + struct.pack('<QIQI', *row[1:]))
    for b in blobs: f.write(b)
print(f'{out}: {len(keys)} shaders, {off / 1e6:.0f} MB, chain {chain}, GL depth variants {sum(bool(r[-1]) for r in idx) if has_gl else 0}')
