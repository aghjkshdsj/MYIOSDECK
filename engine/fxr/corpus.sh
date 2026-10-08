#!/bin/bash
# Instruction and instruction-pair frequencies in a corpus of real x86-64 code that is neither
# benchmark (system binaries of the CI runner, plus any extra files given), to choose FXR's
# superinstructions without looking at the benchmark kernels. Static counts over the disassembly.
#   engine/fxr/corpus.sh [extra binaries...]
set -uo pipefail
files=()
for f in /usr/bin/python3.12 /usr/lib/x86_64-linux-gnu/libc.so.6 /usr/lib/x86_64-linux-gnu/libstdc++.so.6 \
         /usr/lib/gcc/x86_64-linux-gnu/13/cc1 /usr/bin/perl /usr/bin/git /usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
         /usr/bin/bash /usr/lib/x86_64-linux-gnu/libz.so.1 /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 "$@"; do
    f=$(readlink -f "$f" 2>/dev/null) && [ -f "$f" ] && files+=("$f")
done
echo "corpus: ${#files[@]} files"; ls -la "${files[@]}" | awk '{print $5, $NF}'
for f in "${files[@]}"; do objdump -d --no-show-raw-insn -M intel "$f" 2>/dev/null; done | awk '
function kind(o,   r) {
    if (o ~ /\[/) return "m"
    if (o ~ /^(0x[0-9a-f]+|-?[0-9]+)$/) return "i"
    if (o ~ /^xmm/) return "x"
    if (o ~ /^r[0-9]+d$|^e[a-z][a-z]$/) return "r32"
    if (o ~ /^r[0-9]+w$|^[a-ds][xip]$|^[sd]il$|^[bs]pl$/) return "r16"
    if (o ~ /^r[0-9]+b$|^[a-d][lh]$|^[sd]il$|^[bs]pl$/) return "r8"
    if (o ~ /^r[a-z0-9]+$/) return "r64"
    return "?"
}
function reg64(o) {   # canonical register number for dependency checks ("" for non-registers)
    if (o ~ /\[/ || o ~ /^(0x|-?[0-9])/) return ""
    sub(/^e/, "r", o); sub(/[dwb]$/, "", o); return o
}
/^ *[0-9a-f]+:\t/ {
    split($0, f, "\t"); ins = f[2]; gsub(/ +$/, "", ins)
    if (ins == "" || ins ~ /^(\(bad\)|data16|nop|xchg   ax,ax|int3)/) { prev = ""; next }
    n = index(ins, " "); mn = n ? substr(ins, 1, n - 1) : ins; ops = n ? substr(ins, n) : ""
    gsub(/^ +/, "", ops); gsub(/(QWORD|DWORD|WORD|BYTE|XMMWORD) PTR /, "", ops)
    if (mn ~ /^(rep|repz|repnz|lock|bnd|notrack|cs|ds)$/) { mn = mn " " ops; ops = "" }
    na = split(ops, a, ","); d = na >= 1 ? a[1] : ""; s = na >= 2 ? a[2] : ""
    shape = mn (na >= 1 ? " " kind(d) : "") (na >= 2 ? "," kind(s) : "") (na >= 3 ? ",i" : "")
    if (mn ~ /^j[a-z]+$/ && mn != "jmp") shape = "jcc"
    if (mn ~ /^set[a-z]+$/) shape = "setcc " kind(d)
    if (mn ~ /^cmov[a-z]+$/) shape = "cmovcc " kind(d) "," kind(s)
    total++; single[shape]++
    if (prev != "") {
        rel = ""
        if (pd != "" && reg64(d) == pd) rel = " [same dst]"
        else if (pd != "" && (index(s, pd) || (d ~ /\[/ && index(d, pd)))) rel = " [uses dst]"
        pair[prev " ; " shape rel]++; npair++
    }
    if (mn ~ /^(j|call|ret|jmp|ud2|hlt)/) { prev = ""; next }
    prev = shape; pd = reg64(d)
}
END {
    printf "instructions: %d, adjacent pairs inside straight-line code: %d\n\n", total, npair
    print "== top single instruction shapes (per mille)"
    for (k in single) printf "%7.2f  %s\n", 1000 * single[k] / total, k | "sort -rn | head -70"
    close("sort -rn | head -70")
    print "\n== top adjacent pairs (per mille of all instructions)"
    for (k in pair) printf "%7.2f  %s\n", 1000 * pair[k] / total, k | "sort -rn | head -150"
    close("sort -rn | head -150")
}'
