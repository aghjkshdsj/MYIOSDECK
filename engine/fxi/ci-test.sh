#!/bin/bash
# FXI correctness + speed check on an x86-64 Linux host (GitHub Actions):
# every guest runs natively and under FXI; checksums must match.
#   engine/fxi/ci-test.sh <guest-dir> <fxi-binary>
set -uo pipefail
G="$1"; FXI="$2"
fail=0
summary="${GITHUB_STEP_SUMMARY:-/dev/null}"

echo "== hello =="
"$G/hello.elf" > /tmp/native_hello.txt
if "$FXI" "$G/hello.elf" > /tmp/fxi_hello.txt; then
    echo "hello: ok"; cat /tmp/fxi_hello.txt
else
    echo "hello: FAILED"; cat /tmp/fxi_hello.txt; fail=1
fi

# Exact-output tests: atomics (cmpxchg/xadd/xchg/lock ALU/cmpxchg16b/bt*/shld/shrd), x87.
for t in atomics x87 difftest; do
    echo "== $t =="
    "$G/$t.elf" > "/tmp/native_$t.txt"
    "$FXI" "$G/$t.elf" > "/tmp/fxi_$t.txt" 2> "/tmp/fxi_${t}_err.txt"
    if diff -u "/tmp/native_$t.txt" "/tmp/fxi_$t.txt"; then
        echo "$t: match ($(wc -l < "/tmp/native_$t.txt") lines)"
        echo "- $t: **match** (output identical to native)" >> "$summary"
    else
        echo "$t: MISMATCH"; cat "/tmp/fxi_${t}_err.txt"; fail=1
        echo "- $t: **MISMATCH** $(tail -1 "/tmp/fxi_${t}_err.txt")" >> "$summary"
    fi
done

{
    echo "## FXI vs native (x86-64 CI host)"
    echo "| kernel | native | FXI | FXI % of native | checksum |"
    echo "|---|---|---|---|---|"
} >> "$summary"

# Interpreter scale: the app's no-JIT benchmark uses the default scale / 20.
for spec in integer:3 float:16 memory:50 branch:6 simd:150; do
    k="${spec%%:*}"; s="${spec##*:}"
    native=$("$G/bench_sse2.elf" "$k" "$s" | grep RESULT)
    fxi=$("$FXI" "$G/bench_sse2.elf" "$k" "$s" 2> /tmp/fxi_err.txt | grep RESULT || true)
    nns=$(sed -n 's/.*ns=\([0-9]*\).*/\1/p' <<< "$native"); nsum=$(sed -n 's/.*sum=\([0-9]*\).*/\1/p' <<< "$native")
    if [ -z "$fxi" ]; then
        echo "$k: FAILED: $(cat /tmp/fxi_err.txt)"
        echo "| $k | ${nns} ns | failed | - | $(tail -1 /tmp/fxi_err.txt) |" >> "$summary"
        fail=1; continue
    fi
    fns=$(sed -n 's/.*ns=\([0-9]*\).*/\1/p' <<< "$fxi"); fsum=$(sed -n 's/.*sum=\([0-9]*\).*/\1/p' <<< "$fxi")
    pct=$(awk -v n="$nns" -v f="$fns" 'BEGIN { printf "%.2f", (f > 0 ? 100 * n / f : 0) }')
    if [ "$nsum" = "$fsum" ]; then ck="match"; else ck="MISMATCH ($nsum vs $fsum)"; fail=1; fi
    echo "$k: native ${nns} ns, fxi ${fns} ns = ${pct}% of native, checksum $ck"
    echo "| $k | ${nns} ns | ${fns} ns | ${pct}% | $ck |" >> "$summary"
done
exit $fail
