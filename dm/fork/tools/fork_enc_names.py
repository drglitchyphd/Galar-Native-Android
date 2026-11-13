# FORK v48: makes dm/android/app/jni/fork_enc_names.h from PKHeX's English text lists (GPL-3.0). Usage:
#   git clone --depth 1 --filter=blob:none --sparse https://github.com/kwsch/PKHeX /tmp/pkhex && (cd /tmp/pkhex && git sparse-checkout set PKHeX.Core/Resources/text/other/en)
#   python3 dm/fork/tools/fork_enc_names.py /tmp/pkhex/PKHeX.Core/Resources/text/other/en
import sys, unicodedata
base = sys.argv[1].rstrip('/') + '/'
def load(f): return [l.rstrip('\r\n') for l in open(base + f, encoding='utf-8-sig')]
def asc(s):
    s = s.replace('♀', ' F').replace('♂', ' M').replace('’', "'")
    return unicodedata.normalize('NFKD', s).encode('ascii', 'ignore').decode().upper().replace('"', '')
sp = load('text_Species_en.txt')[:899]; ab = load('text_Abilities_en.txt')[:268]
out = ['// FORK v48: species (0-898) and ability (0-267) names in upper-case ASCII for the encounter panel (touch_ui.c), from PKHeX',
       '// (kwsch/PKHeX, PKHeX.Core/Resources/text/other/en: text_Species_en.txt, text_Abilities_en.txt; GPL-3.0, as this app). Made by dm/fork/tools/fork_enc_names.py',
       '#pragma once', 'static const char *const k_enc_species[%d] = {' % len(sp)]
out += ['    ' + ', '.join('"%s"' % asc(x) for x in sp[i:i + 10]) + ',' for i in range(0, len(sp), 10)]
out += ['};', 'static const char *const k_enc_ability[%d] = {' % len(ab)]
out += ['    ' + ', '.join('"%s"' % asc(x) for x in ab[i:i + 10]) + ',' for i in range(0, len(ab), 10)]
out += ['};']
open('dm/android/app/jni/fork_enc_names.h', 'w').write('\n'.join(out) + '\n')
