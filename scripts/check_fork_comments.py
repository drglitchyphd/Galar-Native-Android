#!/usr/bin/env python3
# FORK: flags a FORK line whose // comment swallowed code: after the comment starts, something that looks like a C / Java statement (an assignment, a call ending
# in ';', or a closing '}' followed by code). Run by build_fork.sh before building; a hit stops the build. False alarms: add "// check: ok" to the line.
import os, re, sys, pathlib
if len(sys.argv) < 2: os.chdir(pathlib.Path(__file__).resolve().parent.parent)   # (the repository root; a test passes a folder to run in place)
roots = ['dm/android/app/jni', 'dm/gfx/nvn', 'dm/host', 'dm/android/app/java/com/nxnative']
code_after = re.compile(r'(\b[A-Za-z_][\w\.\[\]>-]*\s*(=|\+=|-=)\s*[^=;]+;)|(\b[A-Za-z_]\w*\([^;]*\)\s*;)|(\}\s*[A-Za-z_]\w*[\s\(])')
hits = 0
for root in roots:
    for p in pathlib.Path(root).rglob('*'):
        if p.suffix not in ('.c', '.h', '.java') or 'third_party' in p.parts: continue
        for n, line in enumerate(p.read_text(encoding='utf-8', errors='ignore').splitlines(), 1):
            if 'FORK' not in line or 'check: ok' in line: continue
            s = re.sub(r'"(\\.|[^"\\])*"', '""', line)   # strings out
            i = s.find('//')
            if i < 0 or not s[:i].strip(): continue   # (a comment-only line cannot swallow code)
            comment = s[i + 2:]
            structural = sum(comment.count(ch) for ch in '{};=')   # (prose rarely has three of these; a swallowed statement or initializer does)
            if (code_after.search(comment) and comment.count(';') >= 2) or structural >= 3 or re.search(r'\bnew \w+(\[\])*\s*[\(\{\[]', comment) \
               or (code_after.search(comment) and comment.rstrip().endswith(';')) or re.search(r'\bif\s*\([^)]*\)\s*[A-Za-z_][\w\.]*\s*(=|\()', comment):   # (v56: + a single statement at the end, an if: the v34 fade line)
                print(f'{p}:{n}: code inside a // comment? {line.strip()[:160]}'); hits += 1
sys.exit(1 if hits else 0)
