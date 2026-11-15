#!/usr/bin/env python3
# Reads a player's debug report (DebugBundle.java: <game>-debug-report.zip) on a PC: the phone / GPU / driver summary from env.json, why the last runs ended,
# Android's native crash records (exits/*.tombstone.pb) decoded into the crashing thread's backtrace, the signal and fault address, and the last pipelines
# built. No protobuf library needed: tombstone.pb (Android 12+, system/core/debuggerd/proto/tombstone.proto) is decoded by hand, the fields used below.
# Usage: read_debug_report.py <debug-report.zip> [--all-threads] [--raw]
import io, json, struct, sys, zipfile

def varint(b, i):
    v = s = 0
    while True:
        c = b[i]; i += 1; v |= (c & 0x7f) << s; s += 7
        if not c & 0x80: return v, i

def fields(b):   # (field number, wire type, value) of one message
    i, out = 0, []
    while i < len(b):
        k, i = varint(b, i); f, w = k >> 3, k & 7
        if w == 0: v, i = varint(b, i)
        elif w == 1: v = struct.unpack_from('<Q', b, i)[0]; i += 8
        elif w == 2: n, i = varint(b, i); v = b[i:i + n]; i += n
        elif w == 5: v = struct.unpack_from('<I', b, i)[0]; i += 4
        else: raise ValueError(f'wire type {w} at {i}')
        out.append((f, w, v))
    return out

def msg(b):   # field number -> list of values
    d = {}
    for f, _, v in fields(b): d.setdefault(f, []).append(v)
    return d

def s(v): return v.decode('utf-8', 'replace') if isinstance(v, (bytes, bytearray)) else str(v)
def one(d, f, default=None): return d.get(f, [default])[0]

# tombstone.proto (the fields read here)
#   Tombstone: arch 1, build_fingerprint 2, revision 3, timestamp 4, pid 5, tid 6, uid 7, command_line 9, signal_info 10, abort_message 14, causes 15,
#              threads 16 (map<uint32, Thread>), memory_mappings 17, log_buffers 18, process_uptime 20
#   Signal: number 1, name 2, code 3, code_name 4, has_fault_address 8, fault_address 9
#   Cause: human_readable 1
#   Thread: id 1, name 2, registers 3, current_backtrace 4, backtrace_note 7
#   Register: name 1, u64 2
#   BacktraceFrame: rel_pc 1, pc 2, sp 3, function_name 4, function_offset 5, file_name 6, file_map_offset 7, build_id 8
#   LogBuffer: name 1, logs 2; LogMessage: timestamp 1, pid 2, tid 3, priority 4, tag 5, message 6
def frame_line(n, fr):
    f = msg(fr); fn = s(one(f, 4, b'')); off = one(f, 5, 0); bid = s(one(f, 8, b''))
    return f'  #{n:02d} pc {one(f, 1, 0):016x}  {s(one(f, 6, b"?"))}' + (f' ({fn}+{off})' if fn else '') + (f' (BuildId: {bid})' if bid else '')

def tombstone(b, all_threads=False, raw=False):
    t = msg(b); out = []
    out.append(f'fingerprint: {s(one(t, 2, b""))}')
    out.append(f'time: {s(one(t, 4, b""))}  pid {one(t, 5)}  tid {one(t, 6)}  uptime {one(t, 20, 0)} s  cmd: {" ".join(s(x) for x in t.get(9, []))}')
    if 10 in t:
        g = msg(one(t, 10)); fa = f'  fault addr {one(g, 9, 0):#x}' if one(g, 8, 0) else ''
        out.append(f'signal {one(g, 1)} ({s(one(g, 2, b""))}), code {one(g, 3)} ({s(one(g, 4, b""))}){fa}')
    if 14 in t: out.append(f'abort message: {s(one(t, 14))}')
    for c in t.get(15, []): out.append(f'cause: {s(one(msg(c), 1, b""))}')
    crash_tid = one(t, 6)
    threads = []
    for e in t.get(16, []):   # map entry: key 1, value 2
        m = msg(e); th = msg(one(m, 2, b'')); threads.append((one(m, 1, 0), th))
    threads.sort(key=lambda x: (x[0] != crash_tid, x[0]))
    for tid, th in threads:
        if tid != crash_tid and not all_threads: continue
        out.append(f'\nthread {tid} "{s(one(th, 2, b""))}"' + (' (crashed)' if tid == crash_tid else ''))
        if tid == crash_tid:
            regs = [(s(one(msg(r), 1, b'')), one(msg(r), 2, 0)) for r in th.get(3, [])]
            for i in range(0, len(regs), 4): out.append('  ' + '  '.join(f'{n:>3} {v:016x}' for n, v in regs[i:i + 4]))
        for n_ in th.get(7, []): out.append(f'  note: {s(n_)}')
        for n, fr in enumerate(th.get(4, [])): out.append(frame_line(n, fr))
    if not all_threads and len(threads) > 1: out.append(f'\n({len(threads) - 1} other threads: --all-threads prints them)')
    for lb in t.get(18, []):
        l = msg(lb); logs = l.get(2, [])
        if not logs: continue
        out.append(f'\nlog buffer {s(one(l, 1, b""))} (last {min(len(logs), 30)} of {len(logs)}):')
        for m_ in logs[-30:]:
            m = msg(m_); out.append(f'  {s(one(m, 1, b""))} {one(m, 2, 0)}/{one(m, 3, 0)} {s(one(m, 5, b""))}: {s(one(m, 6, b"")).rstrip()}')
    if raw:
        out.append('\nraw top-level fields: ' + ', '.join(f'{k}x{len(v)}' for k, v in sorted(t.items())))
    return '\n'.join(out)

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args: print(__doc__ or 'usage: read_debug_report.py <debug-report.zip> [--all-threads] [--raw]'); sys.exit(2)
    allt, raw = '--all-threads' in sys.argv, '--raw' in sys.argv
    z = zipfile.ZipFile(args[0]); names = z.namelist()
    if 'env.json' in names:
        e = json.loads(z.read('env.json'))
        a, g, d, gp, st = e.get('app', {}), e.get('game', {}), e.get('device', {}), e.get('gpu', {}), e.get('state', {})
        print(f'{a.get("name")} {a.get("versionName")} (code {a.get("versionCode")}, {a.get("signer")}, official {a.get("official")})')
        print(f'game: {g.get("name")} {g.get("titleId")} {g.get("version")}  chain {g.get("shaderChain")} (app wants {a.get("shaderChainWanted")})  pack {g.get("packShaders")} complete {g.get("packComplete")}')
        print(f'phone: {d.get("manufacturer")} {d.get("model")} ({d.get("device")})  SoC {d.get("socManufacturer")} {d.get("socModel")}  Android {d.get("android")} (SDK {d.get("sdk")})  page {d.get("pageSize")}  kernel {d.get("kernel")}')
        print(f'GPU: {gp.get("name")}  driver: {gp.get("driver")}' + (f'  sha256 {gp.get("driverSha256")}' if gp.get('driverSha256') else ''))
        for l in gp.get('systemLibraries', []): print(f'  {l.get("path")}  {l.get("size")} B  sha256 {l.get("sha256")}')
        print(f'memory: {st.get("ramTotalMB")} MB, {st.get("ramFreeMB")} MB free  thermal {st.get("thermalStatus")}  battery {st.get("batteryTempC")} °C')
        mods = [m['name'] for m in e.get('mods', []) if m.get('on')]
        print(f'mods on: {", ".join(mods) if mods else "none"}')
    if 'exits/summary.txt' in names: print('\n== why the last runs ended\n' + z.read('exits/summary.txt').decode('utf-8', 'replace'))
    for n in sorted(x for x in names if x.endswith('.tombstone.pb')):
        b = z.read(n); print(f'== {n} ({len(b)} bytes)')
        if b[:3] == b'***' or b[:5] == b'Build': print(b.decode('utf-8', 'replace')[:20000]); continue   # (some phones hand over the text form)
        try: print(tombstone(b, allt, raw))
        except Exception as ex: print(f'(not decodable as tombstone.proto: {ex})')
        print()
    if 'shaders_at_crash.txt' in names: print('== shaders at crash\n' + z.read('shaders_at_crash.txt').decode('utf-8', 'replace'))
    if 'logs/log.prev.txt' in names:
        tail = z.read('logs/log.prev.txt').decode('utf-8', 'replace').splitlines()
        i = max((k for k, l in enumerate(tail) if 'CRASH' in l or 'FATAL' in l), default=-1)
        if i >= 0: print('== engine crash record (log.prev.txt)\n' + '\n'.join(tail[max(0, i - 5):i + 40]))

if __name__ == '__main__': main()
