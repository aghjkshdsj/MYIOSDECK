#!/bin/bash
# Correctness gates for an engine binary (FXR or FXI); any failure fails the job.
#   engine/fxr/gates.sh <guest-dir> <engine> <name> <native-output-dir|run> <pe-dir>
#  - hello runs to exit 0
#  - atomics, x87, difftest: output identical to native line for line ("run": run the guest
#    natively here, x86-64 hosts; else <dir>/<test>.txt from a native run on an x86-64 host)
#  - --scan-pe: every function of the Madeira x64 test programs in <pe-dir> decodes (0 missing)
set -uo pipefail
G="$1"; ENG="$2"; NAME="$3"; NAT="$4"; PE="$5"
summary="${GITHUB_STEP_SUMMARY:-/dev/null}"
fail=0
echo "## $NAME correctness gates ($(uname -m))" >> "$summary"

if timeout 300 "$ENG" "$G/hello.elf" > "/tmp/${NAME}_hello.txt" 2> "/tmp/${NAME}_hello_err.txt"; then
    echo "hello: ok ($(head -1 "/tmp/${NAME}_hello.txt"))"; echo "- hello: ok" >> "$summary"
else
    echo "hello: FAILED: $(tail -2 "/tmp/${NAME}_hello_err.txt")"; echo "- hello: **FAILED**" >> "$summary"; fail=1
fi

for t in atomics x87 difftest; do
    if [ "$NAT" = run ]; then "$G/$t.elf" > "/tmp/native_$t.txt"; ref="/tmp/native_$t.txt"; else ref="$NAT/$t.txt"; fi
    timeout 300 "$ENG" "$G/$t.elf" > "/tmp/${NAME}_$t.txt" 2> "/tmp/${NAME}_${t}_err.txt"
    if [ -s "$ref" ] && cmp -s "$ref" "/tmp/${NAME}_$t.txt"; then
        echo "$t: identical to native ($(wc -l < "$ref") lines)"; echo "- $t: identical to native ($(wc -l < "$ref") lines)" >> "$summary"
    else
        echo "$t: MISMATCH: $(tail -1 "/tmp/${NAME}_${t}_err.txt")"; diff "$ref" "/tmp/${NAME}_$t.txt" | head -20
        echo "- $t: **MISMATCH**" >> "$summary"; fail=1
    fi
done

if ls "$PE"/*.exe > /dev/null 2>&1; then
    timeout 300 "$ENG" --scan-pe "$PE"/*.exe > "/tmp/${NAME}_scan.txt" 2>&1
    grep '^### ' "/tmp/${NAME}_scan.txt"
    total=$(grep -c '^### ' "/tmp/${NAME}_scan.txt"); clean=$(grep -c ' 0 not implemented' "/tmp/${NAME}_scan.txt")
    if [ "$total" -gt 0 ] && [ "$total" = "$clean" ]; then
        echo "scan-pe: $clean/$total programs decode completely"; echo "- scan-pe: $clean/$total Madeira x64 programs decode completely" >> "$summary"
    else
        echo "scan-pe: only $clean/$total programs decode completely"; echo "- scan-pe: **$clean/$total**" >> "$summary"; fail=1
    fi
else
    echo "scan-pe: no PE files in $PE"; echo "- scan-pe: **no test programs**" >> "$summary"; fail=1
fi
exit $fail
