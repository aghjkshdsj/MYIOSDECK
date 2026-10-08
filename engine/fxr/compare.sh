#!/bin/bash
# Speed of FXR and FXI as a share of native, on this host: median of N runs (default 5) with the
# engine order rotated every run, raw nanoseconds, every checksum must match. Two sets:
#   standard  bench_sse2.elf (engine/guest/bench_kernels.h) at the CI interpreter scales
#   held-out  heldout.elf (engine/guest/heldout.c, frozen) at its default scales / 20
#   engine/fxr/compare.sh <guest-dir> <fxr> <fxi> <native-bench> <native-heldout> [runs]
# native-*: a native driver binary (clang -O2 -ffp-contract=off), or "elf" to run the guest itself
# (x86-64 hosts). Prints one "ROW set kernel native fxr fxi" line per kernel and the means.
set -uo pipefail
G="$1"; FXR="$2"; FXI="$3"; NAT_STD="$4"; NAT_HO="$5"; RUNS="${6:-5}"
summary="${GITHUB_STEP_SUMMARY:-/dev/null}"
arch=$(uname -m)
fail=0
field() { sed -n "s/.*$1=\([0-9]*\).*/\1/p"; }
median() { printf '%s\n' "$@" | sort -n | sed -n "$(( ($# + 1) / 2 ))p"; }
pct() { awk -v n="$1" -v e="$2" 'BEGIN { if (e > 0) printf "%.2f", 100 * n / e; else printf "0" }'; }

# Every engine (native too) runs pinned to the same core when taskset exists: shared runners
# migrate processes, and FXR's threaded dispatch is sensitive to that.
PIN=()
if command -v taskset > /dev/null && [ "$(nproc)" -gt 1 ]; then PIN=(taskset -c 1); fi
echo "pinning: ${PIN[*]:-none}"
run_one() {   # engine elf native-driver kernel scale -> RESULT line (empty on failure)
    local e="$1" elf="$2" nat="$3" k="$4" s="$5"
    case $e in
        native) if [ "$nat" = elf ]; then "${PIN[@]}" "$G/$elf" "$k" "$s"; else "${PIN[@]}" "$nat" "$k" "$s"; fi ;;
        fxr) "${PIN[@]}" "$FXR" "$G/$elf" "$k" "$s" 2>> /tmp/fxr_err.txt ;;
        fxi) "${PIN[@]}" "$FXI" "$G/$elf" "$k" "$s" 2>> /tmp/fxi_err.txt ;;
    esac | grep RESULT
}

bench_set() {   # set-name elf native-driver "kernel:scale ..."
    local set="$1" elf="$2" nat="$3" specs="$4"
    local sum_r=0 sum_f=0 count=0
    { echo "### $set set ($arch, median of $RUNS, rotating order)"; echo
      echo "| kernel | native ns | FXR ns | FXR % | FXI ns | FXI % | checksum |"; echo "|---|---|---|---|---|---|---|"; } >> "$summary"
    for spec in $specs; do
        local k="${spec%%:*}" s="${spec##*:}"
        local n=() r=() f=() sums="" engines=(native fxr fxi)
        for ((i = 0; i < RUNS; i++)); do
            for ((j = 0; j < 3; j++)); do
                local e="${engines[$(( (i + j) % 3 ))]}" out
                out=$(run_one "$e" "$elf" "$nat" "$k" "$s") || true
                local ns; ns=$(field ns <<< "$out")
                sums="$sums $(field sum <<< "$out")"
                case $e in native) n+=("${ns:-0}");; fxr) r+=("${ns:-0}");; fxi) f+=("${ns:-0}");; esac
            done
        done
        local uniq; uniq=$(tr ' ' '\n' <<< "$sums" | sed '/^$/d' | sort -u | wc -l)
        local nsum; nsum=$(tr ' ' '\n' <<< "$sums" | sed '/^$/d' | wc -l)
        local nm rm fm; nm=$(median "${n[@]}"); rm=$(median "${r[@]}"); fm=$(median "${f[@]}")
        local ck=match
        if [ "$uniq" != 1 ] || [ "$nsum" != $((3 * RUNS)) ]; then ck="MISMATCH/FAIL"; fail=1; fi
        local rp fp; rp=$(pct "$nm" "$rm"); fp=$(pct "$nm" "$fm")
        sum_r=$(awk -v a="$sum_r" -v b="$rp" 'BEGIN { print a + b }'); sum_f=$(awk -v a="$sum_f" -v b="$fp" 'BEGIN { print a + b }')
        count=$((count + 1))
        echo "ROW $set $k native=$nm fxr=$rm fxi=$fm fxr%=$rp fxi%=$fp checksum=$ck"
        echo "RAW $set $k native=[${n[*]}] fxr=[${r[*]}] fxi=[${f[*]}]"
        echo "| $k | $nm | $rm | **$rp%** | $fm | $fp% | $ck |" >> "$summary"
    done
    local mr mf; mr=$(awk -v a="$sum_r" -v c="$count" 'BEGIN { printf "%.2f", a / c }'); mf=$(awk -v a="$sum_f" -v c="$count" 'BEGIN { printf "%.2f", a / c }')
    echo "MEAN $set ($arch): FXR $mr% | FXI $mf%"
    { echo; echo "**$set mean: FXR $mr% of native, FXI $mf%**"; echo; } >> "$summary"
}

: > /tmp/fxr_err.txt; : > /tmp/fxi_err.txt
bench_set standard bench_sse2.elf "$NAT_STD" "integer:3 float:16 memory:50 branch:6 simd:150"
bench_set held-out heldout.elf "$NAT_HO" \
    "crc32:75 sort:5 hash:10 vm:15 sha256:40 lz77:100 nbody:50 huffman:65 search:100 tree:3 particles:100"
# The CLIs print a stats line per run ("[fxi] ... ok=1 ..."); show anything else.
for e in fxr fxi; do
    if grep -qv ' ok=1 ' "/tmp/${e}_err.txt"; then echo "$e stderr:"; grep -v ' ok=1 ' "/tmp/${e}_err.txt" | sort | uniq -c | head -20; fi
done
exit $fail
