#!/bin/bash
# Diagnostics only. Performance claims come exclusively from measure.py.
set -euo pipefail
ENG="$1"; G="$2"; OUT="$3"
mkdir -p "$OUT"
sudo apt-get install -y -qq linux-tools-common "linux-tools-$(uname -r)" >/dev/null 2>&1 || sudo apt-get install -y -qq linux-tools-generic >/dev/null 2>&1 || true
sudo sysctl -w kernel.perf_event_paranoid=-1 kernel.kptr_restrict=0 || true
PERF=$(find /usr/lib/linux-tools -name perf -type f | head -1)
PERF=${PERF:-perf}
for spec in integer:3 float:16 memory:50 branch:6 simd:150; do
    k=${spec%%:*}; s=${spec##*:}
    sudo "$PERF" stat -o "$OUT/$k.stat" -e cycles,instructions,branches,branch-misses,stall_frontend,stall_backend \
        taskset -c 1 "$ENG" "$G/bench_sse2.elf" "$k" "$s"
    sudo "$PERF" record -q -o "$OUT/$k.data" -e cycles:u \
        taskset -c 1 "$ENG" "$G/bench_sse2.elf" "$k" "$s"
    sudo "$PERF" report -i "$OUT/$k.data" --stdio --sort symbol > "$OUT/$k.report"
    cat "$OUT/$k.stat"
    head -45 "$OUT/$k.report"
done
sudo chmod -R a+rX "$OUT"
