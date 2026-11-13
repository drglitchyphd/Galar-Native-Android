# Encounter info overlay: research notes (Sword 1.3.2)

Research only (2026-11-13). No code written. Goal: show species / shiny / IVs / nature / ability / EVs
of the Pokémon being battled. Target: Sword 1.3.2, title 0100ABF008968000, main build ID
`a3b75bcd3311385aeed67fbeeb79cbb7`.

**Version match is confirmed.** CaptureSight only enables its SwSh views when the main build ID
starts with `A3 B7 5B CD 33 11 38 5A` (Sword), which is our build
(`zaksabeast/CaptureSight` `src/main.cpp:41`). SysBot.NET requires game version `"1.3.2"`
(`PokeDataOffsetsSWSH.cs:11`, checked in `PokeRoutineExecutor8SWSH.cs:95`). So the offsets below
are for exactly our binary (the offsets for Shield 1.3.2 are the same except where marked).

Sources (shallow clones, commit at time of reading):
- SysBot.NET `kwsch/SysBot.NET` @ 844c1f31 (2026-09-08)
  `SysBot.Pokemon/SWSH/Vision/PokeDataOffsetsSWSH.cs` (abbreviated **SB** below)
- CaptureSight `zaksabeast/CaptureSight` @ 3ea02dbd (2023-07-13)
  `libs/csight-core/src/swsh/offsets.rs` (**CS**) and `.../swsh/read.rs`
- PyNXReader `Lincoln-LM/PyNXReader` @ cb15cf59 (2022-06-03) `nxreader/PyNXReader.py` (**PY**)
- sys-botbase `olliz0r/sys-botbase` @ ae13548c `sys-botbase/source/{main.c,commands.c}`
- PKHeX `kwsch/PKHeX` @ 038db512 `PKHeX.Core/PKM/Util/PokeCrypto.cs`, `PKHeX.Core/PKM/Shared/G8PKM.cs`

URLs follow the pattern `https://github.com/<repo>/blob/<commit>/<file>#L<line>`.

## 1. Offsets (all are relative to the HEAP base unless stated otherwise)

Every source below reads these with sys-botbase `peek` / dmnt `new_from_heap`, so each value is
**heap base + offset** (see section 3). None of them is a pointer chain off `main`.

| What | Offset | Read size / format | Source (file:line) | Notes |
|---|---|---|---|---|
| Wild Pokémon (current/last wild battle) | `0x8FEA3648` | 0x148 (PY) or 0x158 (SB), **encrypted** PK8 | SB:42 `WildPokemonOffset`; CS offsets.rs:6 `Wild = 0x8fea3648`; PY:307 `readWild` | All 3 agree. Not cleared after battle (holds the last one, see below). |
| Wild legend (static/overworld legendary) | `0x886BC348` | 0x148, encrypted | SB:44 `LegendaryPokemonOffset` (used for "MotostokeGym" reset mode, EncounterBotResetSWSH.cs:56); PY:313 `readLegend` | **Conflict:** CS offsets.rs:7 `WildLegend = 0x886bc058` (0x2F0 lower). Uncertain which is right for which encounter; test both. |
| Raid Pokémon (in a Max Raid battle) | `0x886C1EC8` | 0x148, encrypted | CS offsets.rs:9 `Raid`; PY:310 `readRaid` | SB has a different one, `RaidPokemonOffset = 0x886A95B8` (SB:43), used for Regigigas / Eternatus resets (EncounterBotResetSWSH.cs:55). |
| Player party slot n (0..5) | `0x450C68B0 + n*0x158` | 0x158 (party format), encrypted | CS offsets.rs:10 `Party`, read.rs:13; PY:292 `readParty` | This is the save block `KParty` (0x2985fe5d) in RAM. |
| Box b slot s (b 0..31, s 0..29) | `0x45075880 + 0x158*(30*b + s)` | 0x158 per slot, encrypted | SB:15 `BoxStartOffset` + `GetBoxSlotOffset` PokeRoutineExecutor8SWSH.cs:20; PY:300 | Box slots in RAM use the 0x158 party size (SB:70 `BoxFormatSlotSize = 0x158`). |
| Current box index (1 byte) | `0x450C680E` | u8 | SB:16 | |
| MyStatus8 (trainer block, 0x110) | `0x45068F18` | TID16 @+0xA0, SID16 @+0xA2, OT name UTF-16 @+0xB0 | SB:17; CS offsets.rs:36 + MyStatus8Offset::SidTid 0xa0; PY:281 | Good sanity check that our heap base is right (OT name is readable). |
| Den list, vanilla (Galar) | `0x450C8A70`, 0x18 per den | RaidSpawnDetail: u64 hash @0, u64 seed @8, stars @0x10, rand roll @0x11, den type @0x12 (0 = inactive), flags @0x13 | CS offsets.rs:16; PY:340; layout: PKHeX `RaidSpawnList8.cs:59-122`, CS `den.rs:27-62` | Isle of Armor `0x450C94D8`, Crown Tundra `0x450C9F40` (CS offsets.rs:17-18). Gives only the seed, not a PK8 (see open questions). |
| In battle / raid flag | `0x3F128624` (Sword), `0x3F128626` (Shield) | u8: 0 when not in battle, 0x40/0x41 in battle | SB:38-39 | Use as "battle active" gate. |
| Battle menu shown | `0x8398A470` | SB compares with screen values | SB:78 | |
| Main RNG state (xoroshiro, 16 bytes) | `0x4C2AAC18` | 2x u64 | CS offsets.rs:24; PY:353 | Not needed for the overlay. |
| Trade partner (link trade) | `0xAF286078` | 0x148/0x158 encrypted | SB:47; CS offsets.rs:8; PY:304 | Not needed. |
| Overworld flag (pointer chain) | `[[main+0x2636678]+0xC0]+0x80` | u8, 1 on overworld | SB:75 `OverworldPointer = [0x2636678, 0xC0, 0x80]` | The only `main`-relative value found. sys-botbase `pointerAll`: deref main+j0, then deref +j1 ..., add the last jump without deref (main.c:605-618, commands.c `followMainPointer`). |

**Not found in any source (opponent party / opponent's active battle Pokémon):** no SysBot,
CaptureSight or PyNXReader offset exists for a trainer battle's opponent party or for the battle
engine's per-slot Pokémon. A web search found nothing either. Only wild / legend / raid singles
are documented. Treat "opponent party" as unknown.

Wild slot behavior (from SysBot logic, not documented explicitly): SysBot waits until
`Species != 0 && ChecksumValid` (`SysBot.Pokemon/Actions/PokeRoutineExecutor.cs:21-33`), and the
reset bot detects a new encounter by comparing with the previous read
(EncounterBotResetSWSH.cs:16-26). So the slot keeps the previous wild Pokémon after the battle
ends; gate display on the in-battle flag and/or EC change.

## 2. PK8 format and decryption (PKHeX)

Sizes (PokeCrypto.cs:49-51): stored `0x148` = 8-byte header + 4 blocks of `0x50`;
party `0x158` = stored + 0x10 bytes of battle stats.

All in-RAM copies above are **encrypted** (CaptureSight reads them as `Ek8` and converts to Pk8,
read.rs:13-39; SysBot passes the bytes to `new PK8(data)`, which decrypts when encrypted).
PKHeX's "is encrypted" test (PokeCrypto.cs:427): `u16 @0x70 != 0 || u16 @0x110 != 0`
(nickname and OT string terminators are 0 in plain data). Use it so plain data also works.

```
// PokeCrypto.Decrypt8 (PokeCrypto.cs:245-256), CryptArray (327-337), Shuffle (354+)
void crypt(u8 *p, size_t len, u32 seed) {          // XOR stream, 16-bit words, little endian
    for (size_t i = 0; i < len; i += 2) {
        seed = seed * 0x41C64E6D + 0x6073;
        *(u16 *)(p + i) ^= (u16)(seed >> 16);
    }
}
void decrypt8(u8 *d, size_t size /* 0x148 or 0x158 */) {
    u32 ec = rd32(d + 0);                            // encryption constant, never encrypted
    u32 sv = (ec >> 13) & 31;
    crypt(d + 8, 0x140, ec);                         // blocks, seed = EC
    if (size > 0x148) crypt(d + 0x148, 0x10, ec);    // party stats: NEW stream, seed = EC again
    // unshuffle: decrypted block i = encrypted block BlockPosition[sv*4 + i]
    u8 tmp[0x140]; memcpy(tmp, d + 8, 0x140);
    for (int i = 0; i < 4; i++)
        memcpy(d + 8 + i*0x50, tmp + BlockPosition[sv*4 + i]*0x50, 0x50);
}
// BlockPosition (PokeCrypto.cs:62-75), 32 entries of 4 (24 permutations + first 8 repeated):
// 0123 0132 0213 0312 0231 0321 1023 1032 2013 3012 2031 3021
// 1203 1302 2103 3102 2301 3201 1230 1320 2130 3120 2310 3210
// 0123 0132 0213 0312 0231 0321 1023 1032
// Validate: checksum = sum of u16 words over [0x08, 0x148) (wrapping u16) == u16 @0x06
//           (G8PKM.cs:28,95); also Sanity u16 @0x04 should be 0 (G8PKM.cs:44,94).
```

Field offsets in decrypted data (G8PKM.cs line numbers):

| Field | Offset | Decode | Line |
|---|---|---|---|
| Encryption constant (EC) | 0x00 | u32 | 93 |
| Sanity / Checksum | 0x04 / 0x06 | u16 / u16 | 94-95 |
| Species | 0x08 | u16 (national dex no.) | 99 |
| Held item | 0x0A | u16 | 100 |
| TID16 / SID16 | 0x0C / 0x0E | u16 / u16 (ID32 = u32 @0x0C) | 101-103 |
| Ability | 0x14 | u16 ability ID | 105 |
| Ability number | 0x16 | `& 7` (1, 2, 4 = hidden); bit 4 = Gigantamax | 106, 108 |
| PID | 0x1C | u32 | 113 |
| Nature | 0x20 | u8 (0-24) | 114 |
| Stat nature (mint) | 0x21 | u8 | 115 |
| Gender | 0x22 | `(b >> 2) & 3`; bit 0 = fateful | 116-118 |
| Form | 0x24 | u16 (getter reads byte) | 121 |
| EVs | 0x26..0x2B | u8 each, order **HP, Atk, Def, Spe, SpA, SpD** | 122-127 |
| Nickname | 0x58 | UTF-16, 13 chars | 47 |
| Move 1..4 | 0x72.. | u16 x4 | 321 |
| IV32 | 0x8C | u32: HP bits 0-4, Atk 5-9, Def 10-14, **Spe 15-19, SpA 20-24, SpD 25-29**, egg bit 30, nicknamed bit 31 | 342-350 |
| Dynamax level | 0x90 | u8 | 352 |
| OT name | 0xF8 | UTF-16 | 49 |
| Party: level | 0x148 | u8 | 450 |
| Party: HP max, Atk, Def, Spe, SpA, SpD | 0x14A..0x154 | u16 each | 452-457 |
| Party: current HP | 0x8A | u16 (in the stored part) | 340 |

Shiny (PKM.cs:318-326, G8PKM.cs:59-60): `x = ID32 ^ PID; xor = (x ^ (x >> 16)) & 0xFFFF;`
shiny if `xor < 16`; Gen 8 square shiny if `xor == 0`, star otherwise. Note: the wild PK8 carries
the *player's* TID/SID (the game builds it as if caught), so this test is valid for wild encounters.
(That last point is an inference from SysBot/CaptureSight showing shiny state for wild PK8; not
stated in source.)

## 3. Heap base: real hardware vs our host

Real hardware:
- sys-botbase `peek <off>` reads `meta.heap_base + off` (main.c:173), where
  `getHeapBase()` = `svcGetInfo(InfoType_HeapRegionAddress = 4, process)` (commands.c:71-78).
  `peekMain` uses the main NSO base from `ldrDmntGetProcessModuleInfo` (commands.c:51-69).
- CaptureSight `DmntReader::new_from_heap(off)` reads `dmntchtMemoryBase().heap + off`
  (csight-core `dmntcht.rs:17-21, 114-117`). Atmosphere dmnt fills `heap` from the same
  HeapRegionAddress info (from memory of Atmosphere source, not re-read here: uncertain).
- On Horizon the heap that `svcSetHeapSize` returns always starts at the heap region start, so
  "heap base" = the address `SetHeapSize` returned = GetInfo(4).

Our host (dm/host/hle_svc.c): guest code runs natively, guest addresses are host pointers.
- `SetHeapSize` returns `g_heap` (hle_svc.c:165) and `GetInfo(I_HeapAddr=4)` also returns `g_heap`
  (hle_svc.c:297). So the game sees the same base in both places, as on hardware.
- For Sword: `g_heap = g_as + 0x10000000 + mem_alias + mem_stack = g_as + 0x30000000`
  (game.tsv: alias 0x10000000, stack 0x10000000, reserve 0x100000000), capacity 0xD0000000, which
  covers the largest offset used here (0xAF286078).
- Translation is therefore just `ptr = (u8 *)g_heap + offset`; guard with
  `dm_guest_heap_readable(ptr, len)` (hle_svc.c:24, it checks against the currently mapped size).
  `g_heap` is `static` in hle_svc.c: a fork accessor would be needed (or read GetInfo 4's value).
  The `main` module base (`g_mod[i].base`) is only needed for the overworld pointer chain.

Why the offsets *should* carry over, and the risk: heap offsets work across all consoles because
the game's allocator layout is deterministic for a given binary and heap size. On our host it holds
only if the game requests the same heap size and makes the same allocations in the same order.
Things that could shift it: a different `SetHeapSize` size (depends on what GetInfo TotalMemory /
app_pool reports, here 0xCD500000), HLE services that allocate through guest memory differently
from real sysmodules, or patches/mods. **Not verified on our host.**

Validation plan (cheap, before any UI):
1. Log `[FORK]` with `g_heap` and the SetHeapSize size once.
2. Read `heap+0x45068F18+0xB0` (OT name) and `+0xA0` (TID/SID) and log them: must match the save.
3. Decrypt party slot 0 at `heap+0x450C68B0` (0x158) and check the checksum + species.
4. In a wild battle, decrypt `heap+0x8FEA3648`, check checksum, compare species with screen.
If 2-3 fail, the heap layout differs: fall back to scanning the heap for the block (e.g. find the
OT name / TID pattern of MyStatus) and derive a delta; the save blocks are contiguous so one delta
may fix all `0x450xxxxx` offsets, but the battle-side ones (`0x8...`) would need their own search
(scan for 0x148-byte spans that decrypt with a valid checksum while the battle flag is set).

## Open questions
- Opponent trainer party / battle engine slots: no known offset. Would need our own search
  (checksum-valid PK8 scan during a trainer battle), and those copies may be a different struct.
- Wild legend: `0x886BC348` (SB, PY) vs `0x886BC058` (CS). Which encounters use which is unknown.
- Raid: `0x886C1EC8` (CS, PY) vs `0x886A95B8` (SB, Regigigas/Eternatus). Probably different
  encounter kinds (den raid vs story/static raid-style battle); untested.
- Den data holds only the seed; the raid Pokémon's IVs/nature come from the seed + nest tables
  (CaptureSight `den.rs`, `den_encounter.rs`, `sword_nests.rs`). The in-battle raid slot is simpler.
- Horde / double wild battles: only one wild slot is documented.
- Whether our host's heap layout matches hardware (section 3) is the main risk.
- Atmosphere dmnt's `heap` base source was not re-read (sys-botbase's was).
