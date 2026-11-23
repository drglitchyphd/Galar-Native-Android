#!/bin/bash
# Simulated-device runs: a Vulkan test binary once on plain lavapipe and once per profile in profiles/ (Khronos profiles layer between the test and lavapipe), the
# Khronos validation layer on in every run. Per run: exit status, the test's FAIL lines, and validation errors / warnings by VUID; then one summary table.
#   bash run_simdev.sh <test binary> [args...]       one binary
#   bash run_simdev.sh all                           builds and runs t_vkdev (twice: as recorded, and held: DM_VK_FRAME_HOLD=2), t_fgen_vk and t_lshud_vk (t_lsfg_vk too
#                                                    when LSFG_SHADERS=<folder of its .spv>)
# Environment: PROFILES="mali-g720-r44p1 xclipse-920" (default: all, plus "lavapipe" for the plain run); OUT=<folder for logs> (default /tmp/simdev-out);
#   VKP=<layers prefix> (default ~/vkprofiles, from setup_layers.sh); TIMEOUT=<seconds per run> (default 300).
#   PROFILE_DIRS=<dir>[:<dir>...]: more folders of profiles beside profiles/ (discover.py's cache); JOBS=<n>: runs at once (default 1; ~8 on 16 GB).
#   With more than 60 profiles the summary lists only the runs that are not clean, plus counts per result (every row stays in rows.tsv).
# t_vkdev is given SIMDEV_PROFILE=<name> so it can check the engine's decisions against that GPU's known facts.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
VKP=${VKP:-$HOME/vkprofiles}; OUT=${OUT:-/tmp/simdev-out}; TIMEOUT=${TIMEOUT:-300}
ICD=${ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}
LAYERS="$VKP/share/vulkan/explicit_layer.d"
[ -f "$LAYERS/VkLayer_khronos_profiles.json" ] && [ -f "$LAYERS/VkLayer_khronos_validation.json" ] || { echo "no layers in $LAYERS: run setup_layers.sh first"; exit 2; }
[ -f "$ICD" ] || { echo "no lavapipe ICD at $ICD"; exit 2; }
mkdir -p "$OUT"
PROFILE_DIRS="$HERE/profiles${PROFILE_DIRS:+:$PROFILE_DIRS}"; JOBS=${JOBS:-1}
ALL_PROFILES="lavapipe $(IFS=:; for d in $PROFILE_DIRS; do [ -d "$d" ] && (cd "$d" && ls *.json 2>/dev/null | sed 's/\.json$//'); done | tr '\n' ' ')"
profile_file() { local IFS=:; for d in $PROFILE_DIRS; do [ -f "$d/$1.json" ] && { echo "$d/$1.json"; return; }; done; }
PROFILES=${PROFILES:-$ALL_PROFILES}

# one run: $1 profile (or lavapipe), $2 label, rest the command. Writes $OUT/<label>.<profile>.{log,vvl} and one row to $OUT/rows.tsv
run_one() {
    local prof=$1 label=$2; shift 2
    local log="$OUT/$label.$prof.log" vvl="$OUT/$label.$prof.vvl"
    rm -f "$log" "$vvl"
    (
        export VK_ICD_FILENAMES="$ICD" VK_DRIVER_FILES="$ICD" VK_ADD_LAYER_PATH="$LAYERS"
        export LD_LIBRARY_PATH="$VKP/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"   # (the installed manifests name the layer libraries without a path)
        export VK_LOADER_DEBUG=
        # Mesa's implicit device-select layer queries Properties2 above the validation layer (a Vulkan 1.1 call on a 1.0 profile that is not the test's): off
        export VK_LOADER_LAYERS_DISABLE=VK_LAYER_MESA_device_select NODEVICE_SELECT=1
        # validation: its default checks (core validation; synchronization and best practices are off by default), errors and warnings into a file of its own
        export VK_KHRONOS_VALIDATION_DEBUG_ACTION=VK_DBG_LAYER_ACTION_LOG_MSG VK_KHRONOS_VALIDATION_LOG_FILENAME="$vvl"
        export VK_KHRONOS_VALIDATION_REPORT_FLAGS=error,warn VK_KHRONOS_VALIDATION_DUPLICATE_MESSAGE_LIMIT=3
        if [ "$prof" = lavapipe ]; then
            export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation SIMDEV_PROFILE=
        else
            # application -> validation -> profiles -> lavapipe: the validation layer judges the application against the simulated device
            export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation:VK_LAYER_KHRONOS_profiles SIMDEV_PROFILE="$prof"
            # the layer loads every profile in its folders: a folder holding a link to this one alone, so runs with thousands cached do not read them all
            local pf pd; pf=$(profile_file "$prof"); [ -n "$pf" ] || { echo "no profile $prof in $PROFILE_DIRS"; exit 3; }
            pd=$(mktemp -d); ln -s "$pf" "$pd/$prof.json"; trap 'rm -rf "$pd"' EXIT
            export VK_KHRONOS_PROFILES_PROFILE_DIRS="$pd"
            eval "$(python3 -I "$HERE/profile_env.py" "$pf")"   # (the profile's name, and the GPU identity the layer does not simulate: SIMDEV_*)
            export VK_KHRONOS_PROFILES_SIMULATE_CAPABILITIES=SIMULATE_API_VERSION_BIT,SIMULATE_FEATURES_BIT,SIMULATE_PROPERTIES_BIT,SIMULATE_EXTENSIONS_BIT,SIMULATE_FORMATS_BIT
            export VK_KHRONOS_PROFILES_EMULATE_PORTABILITY=false VK_KHRONOS_PROFILES_PROFILE_VALIDATION=false
            export VK_KHRONOS_PROFILES_DEBUG_REPORTS=DEBUG_REPORT_ERROR_BIT   # (its warnings list every format lavapipe cannot do as the profile says: hundreds of lines a run)
            export VK_KHRONOS_PROFILES_DEBUG_FAIL_ON_ERROR=false
        fi
        cd "$OUT" && timeout "$TIMEOUT" "$@"
    ) >"$log" 2>&1
    local rc=$?
    local fails; fails=$(grep -c '^FAIL' "$log")
    local verr=0 vwarn=0 vuids=""
    if [ -f "$vvl" ]; then
        verr=$(grep -c -E '^Validation Error' "$vvl"); vwarn=$(grep -c -E '^Validation Warning|^Validation Performance Warning' "$vvl")
        vuids=$(grep -o -E '\[ (VUID|UNASSIGNED)[^] ]*' "$vvl" | sed 's/\[ //' | sort | uniq -c | sort -rn | awk '{printf "%s%s(%s)", (NR>1?" ":""), $2, $1}' | cut -c1-300)
    fi
    local vsim; vsim=$(grep -c -E 'PROFILES (ERROR|WARNING)|VK_LAYER_KHRONOS_profiles.*(ERROR|WARNING)' "$log")
    local res=PASS; [ "$rc" -ne 0 ] && res=FAIL; [ "$rc" -eq 124 ] && res=TIMEOUT; [ "$rc" -ge 128 ] && res="CRASH($((rc-128)))"
    grep -q '^SKIP' "$log" && [ "$rc" -eq 0 ] && res=SKIP
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$label" "$prof" "$res" "$fails" "$verr" "$vwarn" "$vsim" "${vuids:--}" >>"$OUT/rows.tsv"
    printf "  %-10s %-22s %-10s test FAILs %-3s validation errors %-4s warnings %-4s profile-layer msgs %s\n" "$label" "$prof" "$res" "$fails" "$verr" "$vwarn" "$vsim"
}

run_binary() {   # label, command...
    local label=$1; shift
    echo "== $label: $*"
    if [ "$JOBS" -le 1 ]; then for p in $PROFILES; do run_one "$p" "$label" "$@"; done
    else printf '%s\n' $PROFILES | xargs -P "$JOBS" -I{} bash "$0" __one {} "$label" "$@"; fi   # (each run appends one short row: whole lines)
}

build_all() {
    local B=$OUT/bin; mkdir -p "$B"
    bash "$HERE/build_vkdev.sh" "$B/vkdev" >/dev/null || { echo "t_vkdev did not build"; exit 1; }
    local INC="$B/vkdev/inc"   # (build_vkdev.sh made a vulkan.h over the repository's headers)
    local T="$HERE/.."
    gcc -O1 -Wall -Wno-unused-function -I"$INC" -I"$HERE/../../../third_party/vulkan-headers/include" -o "$B/t_fgen_vk" "$T/t_fgen_vk.c" -l:libvulkan.so.1 -lm || exit 1
    gcc -O1 -Wall -Wno-unused-function -I"$INC" -I"$HERE/../../../third_party/vulkan-headers/include" -o "$B/t_lshud_vk" "$T/t_lshud_vk.c" -l:libvulkan.so.1 -lm || exit 1
    if [ -n "${LSFG_SHADERS:-}" ] && [ -d "$LSFG_SHADERS" ]; then
        gcc -O1 -Wall -Wno-unused-function -I"$INC" -I"$HERE/../../../third_party/vulkan-headers/include" -o "$B/t_lsfg_vk" "$T/t_lsfg_vk.c" -l:libvulkan.so.1 -lm || exit 1
    fi
}

if [ "${1:-}" = __one ]; then shift; run_one "$@"; exit 0; fi   # (one run, for JOBS > 1)
rm -f "$OUT/rows.tsv"
if [ "${1:-}" = all ]; then
    build_all
    run_binary t_vkdev "$OUT/bin/vkdev/t_vkdev"
    # again with every submission held and decided (DM_VK_FRAME_HOLD=2, as Faster rendering's Auto runs on Adreno): passes in secondaries, the depth copy's too
    run_binary t_vkdev_held env DM_VK_FRAME_HOLD=2 "$OUT/bin/vkdev/t_vkdev"
    run_binary t_fgen_vk "$OUT/bin/t_fgen_vk"
    run_binary t_lshud_vk "$OUT/bin/t_lshud_vk"
    if [ -x "$OUT/bin/t_lsfg_vk" ]; then run_binary t_lsfg_vk "$OUT/bin/t_lsfg_vk" "$LSFG_SHADERS"; else echo "== t_lsfg_vk: skipped (its shaders are private: LSFG_SHADERS=<folder>)"; fi
elif [ $# -ge 1 ]; then
    run_binary "$(basename "$1")" "$@"
else
    sed -n '2,9p' "$0"; exit 2
fi

echo
echo "Summary ($OUT/rows.tsv; logs: $OUT/<test>.<profile>.log, validation: .vvl)"
sort -o "$OUT/rows.tsv" "$OUT/rows.tsv"
NP=$(cut -f2 "$OUT/rows.tsv" | sort -u | wc -l)
printf "%-11s %-20s %-9s %5s %7s %6s %5s  %s\n" test profile result FAILs vvl-err vvl-wn prof VUIDs
while IFS=$'\t' read -r t p r f e w s v; do
    if [ "$NP" -gt 60 ] && { [ "$r" = PASS ] || [ "$r" = SKIP ]; } && [ "$f" = 0 ] && [ "$e" = 0 ]; then continue; fi
    printf "%-11s %-20s %-9s %5s %7s %6s %5s  %s\n" "$t" "$p" "$r" "$f" "$e" "$w" "$s" "$v"; done <"$OUT/rows.tsv"
if [ "$NP" -gt 60 ]; then
    echo; echo "$NP profiles; runs per test and result:"
    awk -F'\t' '{k=$1" "$3; if ($5>0) k=k" with validation errors"; n[k]++} END {for (k in n) printf "  %-50s %d\n", k, n[k]}' "$OUT/rows.tsv" | sort
    echo "VUIDs (test, VUID, runs that hit it):"
    awk -F'\t' '$8 != "-" {n=split($8, a, " "); for (i=1;i<=n;i++) {v=a[i]; sub(/\(.*/, "", v); c[$1" "v]++}} END {for (k in c) printf "  %-100s %d\n", k, c[k]}' "$OUT/rows.tsv" | sort -k3 -rn | head -40
fi
! grep -q -v -P '\t(PASS|SKIP)\t0\t0\t' "$OUT/rows.tsv"
