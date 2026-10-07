#!/bin/bash
# FXR vs FXI vs native on this host, median of 3 (rotating order), checksums must match.
#   engine/fxr/compare.sh <guest-dir> <fxr> <fxi> <native: "elf" or a native driver binary>
set -uo pipefail
G="$1"; FXR="$2"; FXI="$3"; NATIVE="$4"
summary="${GITHUB_STEP_SUMMARY:-/dev/null}"
fail=0
arch=$(uname -m)
run_native() { if [ "$NATIVE" = elf ]; then "$G/bench_sse2.elf" "$1" "$2"; else "$NATIVE" "$1" "$2"; fi; }
field() { sed -n "s/.*$1=\([0-9]*\).*/\1/p"; }
median() { printf '%s\n' "$@" | sort -n | sed -n 2p; }

for t in hello atomics x87; do
    [ -f "$G/$t.elf" ] || continue
    if "$FXR" "$G/$t.elf" > "/tmp/fxr_$t.txt" 2> "/tmp/fxr_${t}_err.txt"; then echo "$t: fxr ran ok"; else echo "$t: FXR FAILED: $(tail -1 /tmp/fxr_${t}_err.txt)"; fail=1; fi
    if [ "$NATIVE" = elf ] && [ "$t" != hello ]; then
        "$G/$t.elf" > "/tmp/nat_$t.txt"
        if cmp -s "/tmp/nat_$t.txt" "/tmp/fxr_$t.txt"; then echo "$t: output identical to native"; else echo "$t: OUTPUT DIFFERS"; diff "/tmp/nat_$t.txt" "/tmp/fxr_$t.txt" | head -20; fail=1; fi
    fi
done
cat /tmp/fxr_hello.txt

{ echo "## FXR vs FXI vs native ($arch, median of 3)"; echo "| kernel | native | FXR | FXR % | FXI | FXI % | checksum |"; echo "|---|---|---|---|---|---|---|"; } >> "$summary"
sum_fxr=0; sum_fxi=0
for spec in integer:3 float:16 memory:50 branch:6 simd:150; do
    k="${spec%%:*}"; s="${spec##*:}"
    n=(); r=(); f=(); sums=""
    for i in 1 2 3; do
        for e in native fxr fxi; do
            case $e in
                native) out=$(run_native "$k" "$s" | grep RESULT); n+=("$(field ns <<< "$out")");;
                fxr) out=$("$FXR" "$G/bench_sse2.elf" "$k" "$s" 2>/tmp/fxr_err.txt | grep RESULT) || true; r+=("$(field ns <<< "$out")");;
                fxi) out=$("$FXI" "$G/bench_sse2.elf" "$k" "$s" 2>/dev/null | grep RESULT) || true; f+=("$(field ns <<< "$out")");;
            esac
            sums="$sums $(field sum <<< "$out")"
        done
    done
    uniq=$(tr ' ' '\n' <<< "$sums" | sed '/^$/d' | sort -u | wc -l)
    nm=$(median "${n[@]}"); rm=$(median "${r[@]}"); fm=$(median "${f[@]}")
    if [ "$uniq" != 1 ] || [ -z "$rm" ]; then ck="MISMATCH/FAIL: $(tail -1 /tmp/fxr_err.txt)"; fail=1; else ck=match; fi
    rp=$(awk -v a="$nm" -v b="$rm" 'BEGIN { printf "%.2f", (b > 0 ? 100 * a / b : 0) }')
    fp=$(awk -v a="$nm" -v b="$fm" 'BEGIN { printf "%.2f", (b > 0 ? 100 * a / b : 0) }')
    sum_fxr=$(awk -v a="$sum_fxr" -v b="$rp" 'BEGIN { print a + b }'); sum_fxi=$(awk -v a="$sum_fxi" -v b="$fp" 'BEGIN { print a + b }')
    echo "$k: native $nm ns | FXR $rm ns = $rp% | FXI $fm ns = $fp% | checksum $ck"
    echo "| $k | $nm | $rm | **$rp%** | $fm | $fp% | $ck |" >> "$summary"
done
mr=$(awk -v a="$sum_fxr" 'BEGIN { printf "%.2f", a / 5 }'); mf=$(awk -v a="$sum_fxi" 'BEGIN { printf "%.2f", a / 5 }')
echo "MEAN ($arch): FXR $mr% | FXI $mf%"
echo "**Mean: FXR $mr% of native, FXI $mf%**" >> "$summary"
exit $fail
