#!/bin/bash
# FXI32 correctness + speed check on an x86-64 Linux host (docs/NO_JIT_WOW64.md, stage 2):
# every i386 guest runs natively and under fxi32 (inside a 4 GB guest window); outputs must match.
#   engine/fxi32/ci-test.sh <guest32-dir> <fxi32-binary>
set -uo pipefail
G="$1"; FXI="$2"
fail=0
summary="${GITHUB_STEP_SUMMARY:-/dev/null}"
echo "## FXI32 (i386 guests in a 4 GB window) vs native" >> "$summary"

# hello reports the CPU it sees (FXI's CPUID identity): it has to run, not match.
echo "== hello =="
if "$FXI" "$G/hello.elf" > /tmp/fxi32_hello.txt 2>&1; then
    echo "hello: ok"; cat /tmp/fxi32_hello.txt
    echo "- hello: **ok**" >> "$summary"
else
    echo "hello: FAILED"; cat /tmp/fxi32_hello.txt; fail=1
    echo "- hello: **FAILED** $(tail -1 /tmp/fxi32_hello.txt)" >> "$summary"
fi

for t in difftest32; do
    echo "== $t =="
    "$G/$t.elf" > "/tmp/native32_$t.txt"
    "$FXI" "$G/$t.elf" > "/tmp/fxi32_$t.txt" 2> "/tmp/fxi32_${t}_err.txt"
    if diff -u "/tmp/native32_$t.txt" "/tmp/fxi32_$t.txt"; then
        echo "$t: match ($(wc -l < "/tmp/native32_$t.txt") lines)"
        echo "- $t: **match** (output identical to native)" >> "$summary"
    else
        echo "$t: MISMATCH"; cat "/tmp/fxi32_${t}_err.txt"; fail=1
        echo "- $t: **MISMATCH** $(tail -1 "/tmp/fxi32_${t}_err.txt")" >> "$summary"
    fi
done

{
    echo ""
    echo "| kernel | native | FXI32 | FXI32 % of native | checksum |"
    echo "|---|---|---|---|---|"
} >> "$summary"
# bench: SSE2 float math (gated). bench_x87: x87 float math, reported only (FXI keeps x87 values
# as doubles, docs/FAST_INTERPRETER.md: the float kernel may differ in the last bits).
for b in bench bench_x87; do
    for spec in integer:3 float:16 memory:50 branch:6 simd:150; do
        k="${spec%%:*}"; s="${spec##*:}"
        native=$("$G/$b.elf" "$k" "$s" | grep RESULT)
        fx=$("$FXI" "$G/$b.elf" "$k" "$s" 2> /tmp/fxi32_err.txt | grep RESULT || true)
        nns=$(sed -n 's/.*ns=\([0-9]*\).*/\1/p' <<< "$native"); nsum=$(sed -n 's/.*sum=\([0-9]*\).*/\1/p' <<< "$native")
        if [ -z "$fx" ]; then
            echo "$b $k: FAILED: $(cat /tmp/fxi32_err.txt)"
            echo "| $b $k | ${nns} ns | failed | - | $(tail -1 /tmp/fxi32_err.txt) |" >> "$summary"
            fail=1; continue
        fi
        fns=$(sed -n 's/.*ns=\([0-9]*\).*/\1/p' <<< "$fx"); fsum=$(sed -n 's/.*sum=\([0-9]*\).*/\1/p' <<< "$fx")
        pct=$(awk -v n="$nns" -v f="$fns" 'BEGIN { printf "%.2f", (f > 0 ? 100 * n / f : 0) }')
        if [ "$nsum" = "$fsum" ]; then ck="match"
        elif [ "$b" = bench_x87 ] && [ "$k" = float ]; then ck="differs (x87 as double, not gated)"
        else ck="MISMATCH ($nsum vs $fsum)"; fail=1; fi
        echo "$b $k: native ${nns} ns, fxi32 ${fns} ns = ${pct}% of native, checksum $ck"
        echo "| $b $k | ${nns} ns | ${fns} ns | ${pct}% | $ck |" >> "$summary"
    done
done
exit $fail
