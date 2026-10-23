#!/bin/bash
# Soak test: long unattended runs over every lot of the four neighborhoods to
# find crashes, hangs, dead Mains and stuck Sims. For each lot it runs
#   1. s2pkg autonomy <lot> --ticks N --every 1800   (N = SOAK_TICKS, default 129600 = 3 game days)
#   2. s2pkg lot-save <lot> <scratch>/out.package --ticks 300 --check
#   3. the app: --lot <lot> --vm --ui live --run-vm 600 --screenshot x.png --no-audio
# Nobody plays during a soak: the dialogs the trees put up time out as the
# game's (their last button after 4000 + 40 x length ms, on game time
# headless; vm::autoTimeOutDialogs). SOAK_DIALOG=N answers button N at once
# instead (--dialog-answer). The newborn's name dialog (type 23) waits for
# the player in the game too: the soak names the babies after a game minute
# (autonomy --name-babies), as a player would.
# and records: crashes (signal exit, with an lldb backtrace), hangs (wall time
# above SOAK_HANG seconds; a background watchdog kills the job), Mains that
# died (S2PKG_ERRORS=1) or were reset after an error, Sims in the same interaction for more than 6 game
# hours, motives pinned at -100 for more than 6 game hours, save round trips
# that differ and the instruction budget being hit. Scratch saves and
# screenshots are deleted after each check.
#
# usage: tools/soak.sh [--out DIR] [--report FILE] [--report-only] [lot.package ...]
# env:   S2PKG  (build/s2pkg)            APP (build/opensims2.app/Contents/MacOS/opensims2)
#        DATA   (data/game)               SOAK_TICKS (129600)
#        SOAK_JOBS (4)                    SOAK_HANG (seconds per command, default 900; app 300)
#        SOAK_KEEP_LOGS=1 keeps the autonomy logs in OUT/logs/<lot>.log.
#        SOAK_NO_APP=1 skips the app run. SOAK_NO_LLDB=1 skips the backtrace.
#        SOAK_DIALOG (-1): the button every dialog gets at once; -1 lets them time out.
# The report is a markdown table (docs/soak-report.md by default); the raw
# per-lot findings stay in OUT/results/*.txt.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
S2PKG="${S2PKG:-$ROOT/build/s2pkg}"
APP="${APP:-$ROOT/build/opensims2.app/Contents/MacOS/opensims2}"
DATA="${DATA:-$ROOT/data/game}"
SOAK_TICKS="${SOAK_TICKS:-129600}"
SOAK_JOBS="${SOAK_JOBS:-4}"
SOAK_HANG="${SOAK_HANG:-900}"
STUCK_TICKS=${SOAK_STUCK_TICKS:-10800}   # 6 game hours at 30 ticks per game minute
EVERY=1800          # one motive table per game hour
SOAK_DIALOG="${SOAK_DIALOG:--1}"
DIALOG_ARGS=(); [ "$SOAK_DIALOG" != "-1" ] && DIALOG_ARGS=(--dialog-answer "$SOAK_DIALOG")

# run_limited <seconds> <outfile> cmd...: exit status of cmd; 999 if the watchdog killed it.
run_limited() {
    local limit=$1 out=$2; shift 2
    local flag="$out.hang"
    rm -f "$flag"
    "$@" >"$out" 2>&1 &
    local pid=$!
    ( sleep "$limit"; if kill -0 "$pid" 2>/dev/null; then : >"$flag"; kill -9 "$pid" 2>/dev/null; fi ) >/dev/null 2>&1 &
    local wd=$!
    wait "$pid" 2>/dev/null
    local rc=$?
    pkill -P "$wd" 2>/dev/null; kill "$wd" 2>/dev/null; wait "$wd" 2>/dev/null
    if [ -e "$flag" ]; then rm -f "$flag"; return 999; fi
    return $rc
}

backtrace() { # <outfile> cmd... : rerun under lldb for the stack of a crash
    local out=$1; shift
    [ -n "${SOAK_NO_LLDB:-}" ] && return
    run_limited "$SOAK_HANG" "$out" lldb -b -o run -o bt -o quit -- "$@"
    grep -E "stop reason|frame #" "$out" | head -25
}

one() { # one <lot> <outdir>
    local lot=$1 out=$2 name; name=$(basename "$lot" .package)
    local scratch="$out/scratch/$name"; mkdir -p "$scratch" "$out/results"
    local res="$out/results/$name.txt"; : >"$res"
    local t0 t1 rc log

    # 1. autonomy
    log="$scratch/auto.log"; t0=$(date +%s)
    S2PKG_ERRORS=1 run_limited "$SOAK_HANG" "$log" "$S2PKG" autonomy "$lot" --ticks "$SOAK_TICKS" --every "$EVERY" --name-babies ${DIALOG_ARGS[@]+"${DIALOG_ARGS[@]}"}; rc=$?
    t1=$(date +%s); echo "auto_secs=$((t1-t0))" >>"$res"
    if [ $rc -eq 999 ]; then echo "hang|autonomy|killed after ${SOAK_HANG}s; last: $(tail -c 300 "$log" | tr '\n' ' ')" >>"$res"
    elif grep -q "^empty lot package" "$log"; then
        # The empty lot package (the 96-byte stub, N002_Lot32): the game loads
        # it as a lot with nothing in it, so it is not a load error.
        echo "empty|autonomy|$(grep -m1 '^empty lot package' "$log")" >>"$res"
    elif [ $rc -ne 0 ] && [ $rc -lt 128 ] && ! grep -q "^after " "$log"; then
        echo "error|autonomy|exit $rc: $(grep -m1 -E '^error' "$log")" >>"$res"
    elif [ $rc -ge 128 ]; then
        echo "crash|autonomy|exit $rc: $(tail -n 2 "$log" | tr '\n' ' ')" >>"$res"
        backtrace "$scratch/lldb1.log" "$S2PKG" autonomy "$lot" --ticks "$SOAK_TICKS" --every "$EVERY" --name-babies ${DIALOG_ARGS[@]+"${DIALOG_ARGS[@]}"} | sed 's/^/bt|autonomy|/' >>"$res"
    fi
    # the household's Mains that died ("  Name#id  N searches, M picks; its Main died: ..."; visitors are "also:")
    grep -E "^  [^ ].* searches, .* picks; its Main died: " "$log" | sed -E 's/^ *([^ ]+) .*its Main died: /dead|autonomy|\1: /' >>"$res"
    # the household's Mains reset after an error (the game resets the object; VM::resetAfterError)
    grep -E "^  [^ ].* searches, .* picks.*; its Main was reset " "$log" | sed -E 's/^ *([^ ]+) .*its Main was reset ([0-9]+) times? after an error, first: /reset|autonomy|\1: \2 x /' >>"$res"
    grep -c "instruction budget exhausted" "$log" | sed 's/^/budget_autonomy=/' >>"$res"
    # stuck in one interaction, and motives pinned at -100, from the log
    awk -v total="$SOAK_TICKS" -v limit="$STUCK_TICKS" -v every="$EVERY" '
        /^  tick +[0-9]+ [0-9:]+ .* (running|done with) "/ {
            tick=$2+0; name=$4; if (name ~ /^[0-9]+:[0-9]+$/) name=$4; else name=$4
            if ($0 ~ / running "/) { start[name]=tick; what[name]=$0; sub(/.* running "/,"",what[name]); sub(/"$/,"",what[name]) }
            else { if (name in start && tick-start[name] > limit) printf "stuck|autonomy|%s in \"%s\" for %.1f game hours\n", name, what[name], (tick-start[name])/1800; delete start[name] }
        }
        # a Sim removed from the lot (the dead, a child the social worker takes) ends what it was doing
        / is gone \(removed from the lot\)$/ { if (what[$4] != "Falecer") delete start[$4] }
        /^  motives at tick|^after / { intable=1; next }
        intable && NF >= 10 && $1 ~ /#[0-9]+$/ {
            n=split("mood energy comfort hunger hygiene bladder environment social fun", mn, " ")
            for (k=1;k<=9;k++) { v=$(NF-9+k); key=$1 SUBSEP k
                if (v == -100) { run[key]++; if (run[key] > 6 && !(key in said)) { said[key]=1; printf "pinned|autonomy|%s %s at -100 for more than 6 game hours\n", $1, mn[k] } } else run[key]=0 }
        }
        /^  tick|^[A-Za-z]/ { if ($0 !~ /^  motives/) intable=0 }
        END { for (n in start) if (total-start[n] > limit) {
                  # "Falecer" (die) is a death; "Leave" means the Sim left the lot and its id is reused.
                  if (what[n] == "Leave") continue
                  printf "%s|autonomy|%s in \"%s\" since tick %d (%.1f game hours at the end)\n", (what[n] == "Falecer" ? "died" : "stuck"), n, what[n], start[n], (total-start[n])/1800 } }
    ' "$log" >>"$res"
    if [ -n "${SOAK_KEEP_LOGS:-}" ]; then mkdir -p "$out/logs"; mv "$log" "$out/logs/$name.log"; else rm -f "$log"; fi

    # 2. lot-save round trip
    log="$scratch/save.log"; t0=$(date +%s)
    S2PKG_ERRORS=1 run_limited "$SOAK_HANG" "$log" "$S2PKG" lot-save "$lot" "$scratch/out.package" --ticks 300 --check; rc=$?
    t1=$(date +%s); echo "save_secs=$((t1-t0))" >>"$res"
    if [ $rc -eq 999 ]; then echo "hang|lot-save|killed after ${SOAK_HANG}s" >>"$res"
    elif [ $rc -ge 128 ]; then
        echo "crash|lot-save|exit $rc" >>"$res"
        backtrace "$scratch/lldb2.log" "$S2PKG" lot-save "$lot" "$scratch/out2.package" --ticks 300 --check | sed 's/^/bt|lot-save|/' >>"$res"
    elif grep -q "^empty lot package" "$log"; then
        # The empty lot package (the 96-byte stub): not a load error.
        echo "empty|lot-save|$(grep -m1 '^empty lot package' "$log")" >>"$res"
    elif [ $rc -ne 0 ] && ! grep -q "differs:" "$log"; then
        echo "error|lot-save|exit $rc: $(grep -m1 '^error' "$log")" >>"$res"
    elif [ $rc -ne 0 ]; then
        echo "roundtrip|lot-save|exit $rc: $(awk '/differs:|error|usage|refusing/ {p=1} p' "$log" | head -4 | sed 's/|/ /g' | tr '\n' ';')" >>"$res"
    fi
    grep -E "^ *\[main of|^ *\[init of" "$log" | head -5 | sed 's/^/dead|lot-save|/' >>"$res"
    grep -c "instruction budget exhausted" "$log" | sed 's/^/budget_save=/' >>"$res"
    rm -f "$scratch"/out*.package "$log"

    # 3. app
    if [ -z "${SOAK_NO_APP:-}" ] && [ -x "$APP" ]; then
        log="$scratch/app.log"; t0=$(date +%s)
        OS2_DATA="$DATA" run_limited 300 "$log" "$APP" --lot "$lot" --vm --ui live --run-vm 600 --screenshot "$scratch/x.png" --no-audio ${DIALOG_ARGS[@]+"${DIALOG_ARGS[@]}"}; rc=$?
        t1=$(date +%s); echo "app_secs=$((t1-t0))" >>"$res"
        if [ $rc -eq 999 ]; then echo "hang|app|killed after 300s" >>"$res"
        elif [ $rc -lt 128 ] && [ $rc -ne 0 ]; then
            echo "error|app|exit $rc: $(tail -n 1 "$log")" >>"$res"
        elif [ $rc -ne 0 ] || [ ! -s "$scratch/x.png" ]; then
            echo "crash|app|exit $rc: $(tail -n 2 "$log" | tr '\n' ' ')" >>"$res"
            backtrace "$scratch/lldb3.log" "$APP" --lot "$lot" --vm --ui live --run-vm 600 --screenshot "$scratch/x.png" --no-audio ${DIALOG_ARGS[@]+"${DIALOG_ARGS[@]}"} | sed 's/^/bt|app|/' >>"$res"
        fi
        rm -f "$scratch/x.png" "$log"
    fi
    rm -rf "$scratch"
    echo "done|$name" >>"$res"
}

if [ "${1:-}" = "--one" ]; then one "$2" "$3"; exit 0; fi

OUT="${TMPDIR:-/tmp}/os2-soak"; REPORT="$ROOT/docs/soak-report.md"; LOTS=(); REPORT_ONLY=
while [ $# -gt 0 ]; do
    case "$1" in --out) OUT=$2; shift 2;; --report-only) REPORT_ONLY=1; shift;; --report) REPORT=$2; shift 2;; *) LOTS+=("$1"); shift;; esac
done
if [ ${#LOTS[@]} -eq 0 ]; then
    for h in N001 N002 N003 Tutorial; do
        for f in "$DATA"/TSData/Res/UserData/Neighborhoods/$h/Lots/*.package; do LOTS+=("$f"); done
    done
fi
if [ -z "$REPORT_ONLY" ]; then
    rm -rf "$OUT/results" "$OUT/scratch" "$OUT/logs"; mkdir -p "$OUT/results"
    export SOAK_KEEP_LOGS S2PKG APP DATA SOAK_TICKS SOAK_HANG SOAK_NO_APP SOAK_NO_LLDB SOAK_STUCK_TICKS SOAK_DIALOG
    printf '%s\n' "${LOTS[@]}" | xargs -P "$SOAK_JOBS" -I{} "$0" --one {} "$OUT"
fi

# Report
{
    echo "# Soak report"
    echo
    echo "Generated by \`tools/soak.sh\` on $(date +%F): ${#LOTS[@]} lots, autonomy for $SOAK_TICKS ticks ($((SOAK_TICKS/1800)) game hours, 1800 ticks each), lot-save of 300 ticks with --check, app run of 600 VM ticks. Hang limit ${SOAK_HANG}s (app 300s)."
    echo
    echo "## Counts by category"
    echo
    echo "| Category | Lots | Findings |"
    echo "|---|---|---|"
    for c in crash hang error dead reset stuck died pinned roundtrip empty; do
        l=$(grep -l "^$c|" "$OUT"/results/*.txt 2>/dev/null | wc -l | tr -d ' ')
        n=$(cat "$OUT"/results/*.txt 2>/dev/null | grep -c "^$c|")
        echo "| $c | $l | $n |"
    done
    b=$(cat "$OUT"/results/*.txt | grep -E '^budget_' | awk -F= '{s+=$2} END{print s+0}')
    bl=$(grep -lE '^budget_[a-z]+=[1-9]' "$OUT"/results/*.txt 2>/dev/null | wc -l | tr -d ' ')
    echo "| instruction budget (100000) hit | $bl | $b |"
    echo
    echo "## Findings per lot"
    echo
    echo "| Lot | Category | Where | Detail |"
    echo "|---|---|---|---|"
    for f in "$OUT"/results/*.txt; do
        n=$(basename "$f" .txt)
        grep -E '^(crash|hang|error|dead|reset|stuck|died|pinned|roundtrip|empty)\|' "$f" | sort -u | while IFS='|' read -r c w d; do
            echo "| $n | $c | $w | ${d//|/\\|} |"
        done
        grep -E '^budget_[a-z]+=[1-9]' "$f" | while IFS='=' read -r k v; do echo "| $n | budget | ${k#budget_} | $v times |"; done
    done
    echo
    echo "## Stacks of crashes"
    echo
    for f in "$OUT"/results/*.txt; do grep '^bt|' "$f" | awk -F'|' -v n="$(basename "$f" .txt)" '{print "    " n " " $2 ": " $3}'; done
    echo
    echo "## Timing (seconds, per lot: autonomy / lot-save / app)"
    echo
    for f in "$OUT"/results/*.txt; do
        printf -- '- %s: %s / %s / %s\n' "$(basename "$f" .txt)" "$(sed -n 's/^auto_secs=//p' "$f")" "$(sed -n 's/^save_secs=//p' "$f")" "$(sed -n 's/^app_secs=//p' "$f")"
    done
} >"$REPORT"
echo "report: $REPORT"
