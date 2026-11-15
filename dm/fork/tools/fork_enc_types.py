# FORK v52: makes dm/android/app/jni/fork_enc_types.h (types per species / form) from PKHeX's personal_swsh (GPL-3.0). Usage:
#   git clone --depth 1 --filter=blob:none --sparse https://github.com/kwsch/PKHeX /tmp/pkhex && (cd /tmp/pkhex && git sparse-checkout set PKHeX.Core/Resources/byte/personal)
#   python3 dm/fork/tools/fork_enc_types.py /tmp/pkhex/PKHeX.Core/Resources/byte/personal/personal_swsh
# Entry layout (PersonalInfo8SWSH, 0xB0 bytes): type 1 at 0x06, type 2 at 0x07, FormStatsIndex u16 at 0x1E, FormCount at 0x20
import sys
d = open(sys.argv[1], 'rb').read(); N = len(d) // 0xB0
t1 = [d[i * 0xB0 + 6] for i in range(N)]; t2 = [d[i * 0xB0 + 7] for i in range(N)]
fsi = [d[i * 0xB0 + 0x1E] | d[i * 0xB0 + 0x1F] << 8 for i in range(899)]; fc = [d[i * 0xB0 + 0x20] for i in range(899)]
out = ["// FORK v52: Pokémon types for the encounter panel's ribbon colour, from PKHeX's personal_swsh (kwsch/PKHeX, PKHeX.Core/Resources/byte/personal; GPL-3.0,",
       '// as this app): entry = species, or for a form > 0 (PK8 0x24) k_enc_fsi[species] + form - 1 when form < k_enc_fc[species]. Types: 0 Normal 1 Fighting 2 Flying 3 Poison 4 Ground',
       '// 5 Rock 6 Bug 7 Ghost 8 Steel 9 Fire 10 Water 11 Grass 12 Electric 13 Psychic 14 Ice 15 Dragon 16 Dark 17 Fairy. Made by dm/fork/tools/fork_enc_types.py',
       '#pragma once', 'static const unsigned char k_enc_t1[%d] = {' % N]
for i in range(0, N, 40): out.append('    ' + ','.join(map(str, t1[i:i + 40])) + ',')
out += ['};', 'static const unsigned char k_enc_t2[%d] = {' % N]
for i in range(0, N, 40): out.append('    ' + ','.join(map(str, t2[i:i + 40])) + ',')
out += ['};', 'static const unsigned short k_enc_fsi[899] = {']
for i in range(0, 899, 30): out.append('    ' + ','.join(map(str, fsi[i:i + 30])) + ',')
out += ['};', 'static const unsigned char k_enc_fc[899] = {']
for i in range(0, 899, 40): out.append('    ' + ','.join(map(str, fc[i:i + 40])) + ',')
out += ['};']
open('dm/android/app/jni/fork_enc_types.h', 'w').write('\n'.join(out) + '\n')
