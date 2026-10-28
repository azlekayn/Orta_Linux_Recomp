#!/usr/bin/env bash
# orta_loop.sh -- one full round of the Orta recompilation loop, on Linux.
#
#   1. collect missing function addresses from the last run's log
#   2. if there are new ones, re-run the pipeline (disasm -> func_id -> abi_analysis -> recomp)
#   3. rebuild the executable (quick if nothing changed)
#   4. run it
#   5. print a short summary
#
# This is the POSIX sibling of orta_loop.ps1. The recompiler toolchain itself
# (tools/disasm, func_id, abi_analysis, recomp) is pure Python and already runs
# on Linux; only the PowerShell orchestration needed porting. Build deps:
#
#   bash ../xboxrecomp-main/tools/linux/install_deps.sh
#
# Run from anywhere:
#   bash orta_loop.sh
set -u

ORTA="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLKIT="$(cd "$ORTA/../xboxrecomp-main" && pwd)"
SEEDS="$TOOLKIT/game_files/seeds_manual.json"
LOG="$ORTA/stderr.txt"
SECTIONS='MMATRIX,D3D,D3DX,DSOUND,WMADEC,PSGSFD00,PSGSFD_I,PSGSFD_B,PSGSFD_P,XGRPH,XPP'
PY="${PYTHON:-python3}"

# The toolkit's watchdog is a one-shot timer: after this many seconds it
# dumps the guest stack and ENDS the program, hung or not.
WATCHDOG_SECONDS=86400
RUN_LIMIT_SECONDS=86400
KERNEL_LOG_BUDGET=200

step() { printf '\n=== %s ===\n' "$1"; }

fail() {
    printf '\n%s failed. See the lines above.\n' "$1" >&2
    exit 1
}

# Runs a tool, shows only the last few lines of its output, stops on failure.
run_tool() {
    local name="$1"; shift
    local out rc
    step "$name"
    out="$("$@" 2>&1)"; rc=$?
    if [ $rc -ne 0 ]; then
        printf '%s\n' "$out" | tail -n 25
        fail "$name"
    fi
    printf '%s\n' "$out" | tail -n 2
}

# ---- 0. First-run bootstrap ------------------------------------------------
# The recompiler needs the seed files next to the XBE. Copy them from the game
# project if the toolkit's game_files has not been seeded yet.
mkdir -p "$TOOLKIT/game_files"
for f in seeds_manual.json alias_seeds.json trace_functions.json; do
    if [ -f "$ORTA/seeds/$f" ] && [ ! -f "$TOOLKIT/game_files/$f" ]; then
        cp "$ORTA/seeds/$f" "$TOOLKIT/game_files/$f"
    fi
done

if [ ! -f "$TOOLKIT/game_files/default.xbe" ]; then
    echo "No $TOOLKIT/game_files/default.xbe -- copy the disc's files there first." >&2
    exit 1
fi

# ---- 1. Collect missing functions ------------------------------------------
step 'Collecting missing functions from the last run'

known=()
if [ -f "$SEEDS" ]; then
    known=($(grep -o '"start"[[:space:]]*:[[:space:]]*"0x[0-9A-Fa-f]*"' "$SEEDS" \
        | grep -o '0x[0-9A-Fa-f]*' | tr 'a-f' 'A-F' | sort -u))
fi

found=()
if [ -f "$LOG" ]; then
    # Real code only; a null-pointer call is not a function. Also seed code the
    # game reached that was never translated (a branch past a ret, a jump-table
    # case): "[STUB] code at 0x... skipped".
    found+=($(grep -oE 'Failed to resolve VA (0x[0-9A-Fa-f]+)' "$LOG" \
        | grep -oE '0x[0-9A-Fa-f]+' | tr 'a-f' 'A-F' | sort -u))
    found+=($(grep -oE '\[STUB\] code at 0x([0-9A-Fa-f]{8}) skipped' "$LOG" \
        | grep -oE '0x[0-9A-Fa-f]{8}' | tr 'a-f' 'A-F' | sort -u))
fi

# Keep only addresses in the real code range 0x11000..0x30D000.
in_range() {
    local v=$(( $1 ))   # 0x... is accepted by bash arithmetic
    [ "$v" -ge $((0x11000)) ] && [ "$v" -lt $((0x30D000)) ]
}

new=()
for a in "${found[@]:-}"; do
    [ -z "$a" ] && continue
    in_range "$a" || continue
    hit=0
    for k in "${known[@]:-}"; do [ "$k" = "$a" ] && { hit=1; break; }; done
    [ $hit -eq 0 ] && new+=("$a")
done

all=($(printf '%s\n' "${known[@]:-}" "${new[@]:-}" | grep -v '^$' | sort -u))

printf 'Already known: %d   New from last run: %d   Total: %d\n' \
    "${#known[@]}" "${#new[@]}" "${#all[@]}"

# Branch targets inside other functions: become alias entries, not splits.
export XBOXRECOMP_ALIAS_SEEDS="$TOOLKIT/game_files/alias_seeds.json"

# Re-translate when the translator itself has changed since the C was made.
GEN_STAMP="$ORTA/last_translation.stamp"
translator_changed=0
if [ ! -f "$GEN_STAMP" ]; then
    translator_changed=1
else
    for f in \
        "$TOOLKIT/tools/disasm/functions.py" \
        "$TOOLKIT/game_files/alias_seeds.json" \
        "$TOOLKIT/tools/recomp/lifter.py" \
        "$TOOLKIT/tools/recomp/translator.py" \
        "$TOOLKIT/game_files/trace_functions.json" \
        "$TOOLKIT/game_files/seeds_manual.json" \
        "$ORTA/src/recomp_manual.c"; do
        [ -e "$f" ] || continue
        if [ "$f" -nt "$GEN_STAMP" ]; then translator_changed=1; fi
    done
fi
[ $translator_changed -eq 1 ] && echo 'The translator has been updated since the last translation.'

if [ "${#new[@]}" -gt 0 ] || [ $translator_changed -eq 1 ]; then
    if [ "${#new[@]}" -gt 0 ]; then printf 'New: %s\n' "$(IFS=', '; echo "${new[*]}")"; fi
    # "observed": every seed here is an address the game was seen to reach, so
    # the disassembler keeps it even where its sweep had decoded data as code.
    {
        printf '['
        first=1
        for a in "${all[@]}"; do
            [ $first -eq 1 ] || printf ', '
            first=0
            printf '{"start": "%s", "observed": true}' "$a"
        done
        printf ']\n'
    } > "$SEEDS"

    # ---- 2. Pipeline -------------------------------------------------------
    cd "$TOOLKIT" || fail 'cd toolkit'
    run_tool 'Disassembling (1-2 min)' \
        "$PY" -m tools.disasm game_files/default.xbe --text-only \
        --extra-sections "$SECTIONS" --seed-functions game_files/seeds_manual.json --force
    run_tool 'Labelling functions' "$PY" -m tools.func_id game_files/default.xbe
    run_tool 'Checking calling conventions' "$PY" -m tools.abi_analysis game_files/default.xbe
    if [ -f game_files/trace_functions.json ]; then
        run_tool 'Translating to C (3-4 min)' \
            "$PY" -m tools.recomp game_files/default.xbe --all --split 250 \
            --gen-dir ../orta/src/recomp/gen --exclude-manual ../orta/src/recomp_manual.c \
            --trace-functions game_files/trace_functions.json
    else
        run_tool 'Translating to C (3-4 min)' \
            "$PY" -m tools.recomp game_files/default.xbe --all --split 250 \
            --gen-dir ../orta/src/recomp/gen --exclude-manual ../orta/src/recomp_manual.c
    fi
    date -Is > "$GEN_STAMP"
    cd "$ORTA" || fail 'cd orta'
else
    echo 'Nothing new to translate, going straight to build and run.'
fi

# ---- 3. Build --------------------------------------------------------------
step 'Building the executable (quick if nothing changed)'
pkill -f orta_recomp >/dev/null 2>&1
if [ ! -d "$ORTA/build64" ]; then
    cmake -S "$ORTA" -B "$ORTA/build64" -DCMAKE_BUILD_TYPE=Release \
        || fail 'CMake configure'
fi
build_out="$(cmake --build "$ORTA/build64" -j 2>&1)"
if [ $? -ne 0 ]; then
    printf '%s\n' "$build_out" | grep -E 'error' | head -n 30
    fail 'Build'
fi
echo 'Build OK'
[ -f "$ORTA/build_only.flag" ] && [ "$(cat "$ORTA/build_only.flag")" = 1 ] && {
    rm -f "$ORTA/build_only.flag"
    echo 'Build only: the game was not started.'
    exit 0
}

# ---- 4. Run ----------------------------------------------------------------
step "Running the game (up to ${WATCHDOG_SECONDS}s)"
export RECOMP_WATCHDOG_SECS="$WATCHDOG_SECONDS"
export RECOMP_KERNEL_LOG_BUDGET="$KERNEL_LOG_BUDGET"
# Run the game's graphics commands (the GP translator reaches the D3D8 device,
# which is the OpenGL backend here).
export RECOMP_PB_EXEC=1
# Fire the graphics chip's 60 Hz "vertical blank" interrupt. Without it, D3D's
# Present waits forever for a screen refresh that never comes.
export RECOMP_VBLANK=1
# Report the sound chip's codec as ready and run the emulated audio processor,
# so DirectSound's set-up can get past its first hardware check.
export RECOMP_AC97_READY=1
# Let freed memory actually go back to the heap.
export RECOMP_HEAP_RECLAIM=1
# Sample where every thread spends its time; printed to stderr once a minute.
export RECOMP_PROFILE=1
# Unattended testing: a run that stops drawing for a while ends itself.
export RECOMP_STALL_EXIT_SECS=180
export RECOMP_SEMA_DEVPTR=0x2BD708
# The DSP stub: complete the posted command at once so DDS9 does not spin.
export RECOMP_DSP_ACK=0x802C0810,0x80F80810

rm -f "$ORTA/frame.bmp"*
mkdir -p "$ORTA/texdump"; rm -f "$ORTA/texdump"/*.bmp 2>/dev/null
rm -f "$ORTA/ftrace.flag"

start=$(date +%s)
"$ORTA/build64/orta_recomp" > "$ORTA/stdout.txt" 2> "$LOG" &
pid=$!
( sleep "$RUN_LIMIT_SECONDS"; kill -TERM "$pid" 2>/dev/null ) &
watchdog_killer=$!
wait "$pid"; code=$?
kill "$watchdog_killer" 2>/dev/null
end=$(date +%s)
secs=$((end - start))

# ---- 5. Summary ------------------------------------------------------------
step 'Summary of this run'
case "$code" in
    0) how='the game exited normally' ;;
    3) how='the watchdog ended it (time limit reached)' ;;
    *) how='it crashed or was stopped' ;;
esac
printf 'Ran for %d s, exit code %d: %s\n' "$secs" "$code" "$how"

if [ -f "$LOG" ]; then
    misses=$(grep -oE 'Failed to resolve VA (0x[0-9A-Fa-f]+)' "$LOG" | sort -u | wc -l)
    icalls=$(grep -oE 'icalls so far: [0-9]+' "$LOG" | tail -n 1)
    kcalls=$(grep -oE '\[KERNEL\] #[0-9]+:' "$LOG" | grep -oE '[0-9]+' | sort -n | tail -n 1)
    printf 'System calls logged: %s\n' "${kcalls:-0}"
    [ -n "$icalls" ] && printf 'Indirect calls made: %s\n' "${icalls##*: }"
    printf 'Missing functions this run: %s\n' "$misses"
    printf '\n--- Important lines ---\n'
    grep -E 'ICALL\] Failed|scripted|is not code|called from|CRASH|WATCHDOG|Xbox VA of fault|  in sub_|  regs:|  peek:|WATCH|SEMA|PAD|GRAPH|ABI\]|STUB|APC\]| fps \(' "$LOG" | tail -n 30
    printf '\n--- Last 25 log lines (stack dump hidden) ---\n'
    grep -vE '^[[:space:]]+GS ' "$LOG" | tail -n 25
else
    echo 'No log found.'
fi
