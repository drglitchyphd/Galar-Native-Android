# FORK: a percentage for setup step 5 ("pack the shaders"). The step runs three programs that print nothing to the setup screen
# (vk_meta.py, then spv_pak.py, then spv_pak.py --compress), so this thread watches their work from outside and prints the same
# "PROGRESS done/total" lines that step 4 prints (SetupActivity.runSetup turns them into the bar and the percentage).
# None of those programs is changed. Parts of the step:
#   0-20%    vk_meta.py: one .vkr per shader in the vk folder
#   20-60%   spv_pak.py: reads every .spv / .vkr / .gl.spv (bytes read, /proc/<pid>/io), then writes the raw pack
#   60-100%  spv_pak.py --compress: reads the raw pack, then writes shaders.pak (about 0.3 times its size)
# When /proc/<pid>/io can't be read, the bar still moves at each part's start and with the files' sizes.
import os, sys

TOTAL = 1000

def _count(d, end, skip=None):
    try: return sum(1 for e in os.scandir(d) if e.name.endswith(end) and not (skip and e.name.endswith(skip)))
    except OSError: return 0

def _bytes(d):
    n = 0
    try:
        for e in os.scandir(d):
            if e.name.endswith(('.spv', '.vkr')):
                try: n += e.stat().st_size
                except OSError: pass
    except OSError: pass
    return n

def _size(p):
    try: return os.path.getsize(p)
    except OSError: return 0

def _find(script, compress):
    """the pid of the running `python <script>` (with or without --compress), else None"""
    for p in os.listdir('/proc'):
        if not p.isdigit(): continue
        try:
            with open(f'/proc/{p}/cmdline', 'rb') as f: a = f.read().split(b'\0')
        except OSError: continue
        if any(x.endswith(script.encode()) for x in a) and (b'--compress' in a) == compress: return int(p)
    return None

def _rchar(pid):
    try:
        with open(f'/proc/{pid}/io') as f:
            for l in f:
                if l.startswith('rchar:'): return int(l.split()[1])
    except (OSError, ValueError): pass
    return None

def watch_pack(vk, raw, out, stop, every=2):
    shaders = max(1, _count(vk, '.spv', '.gl.spv')); shown = -1; total_in = None; pid_b = pid_c = None
    print(f'[FORK] step 5 progress: {shaders} shaders', file=sys.stderr, flush=True)
    while True:
        if not pid_c: pid_c = _find('spv_pak.py', True)
        if not pid_b and not pid_c: pid_b = _find('spv_pak.py', False)
        if pid_c:   # part 3, compressing
            rs = max(1, _size(raw)); r = _rchar(pid_c)
            f = 0.85 * min(1, (r or 0) / rs) + 0.15 * min(1, _size(out) / (0.3 * rs))
            done = 600 + int(400 * f)
        elif pid_b:   # part 2, the raw pack (its file appears only once everything is read)
            if total_in is None: total_in = max(1, _bytes(vk))
            r = _rchar(pid_b)
            f = 0.8 * min(1, (r or 0) / total_in) + 0.2 * min(1, _size(raw) / total_in)
            done = 200 + int(400 * f)
        else:   # part 1
            done = int(200 * min(1, _count(vk, '.vkr') / shaders))
        done = min(done, TOTAL - 1)   # 100% is the step's end
        if done > shown: shown = done; print(f'PROGRESS {done}/{TOTAL}', flush=True)
        if stop.wait(every): return
