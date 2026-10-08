#!/bin/bash
# Instruction and instruction-pair frequencies in a corpus of real x86-64 code that is neither
# benchmark (system binaries of the CI runner, plus any extra files given), to choose FXR's
# superinstructions without looking at the benchmark kernels. Static counts over the disassembly,
# for all code and for loop bodies only (instructions between a backward branch's target and the
# branch: a proxy for hot code).
#   engine/fxr/corpus.sh [extra binaries...]
set -uo pipefail
files=()
for f in /usr/bin/python3.12 /usr/lib/x86_64-linux-gnu/libc.so.6 /usr/lib/x86_64-linux-gnu/libstdc++.so.6 \
         /usr/lib/gcc/x86_64-linux-gnu/13/cc1 /usr/bin/perl /usr/bin/git /usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
         /usr/bin/bash /usr/lib/x86_64-linux-gnu/libz.so.1 /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
         /usr/lib/x86_64-linux-gnu/libm.so.6 /usr/lib/x86_64-linux-gnu/libBulletDynamics.so.* \
         /usr/lib/x86_64-linux-gnu/libBulletCollision.so.* /usr/lib/x86_64-linux-gnu/libLinearMath.so.* \
         /usr/lib/x86_64-linux-gnu/libbox2d.so.* /usr/lib/x86_64-linux-gnu/liblua5.4.so.0 \
         /usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0 /usr/lib/x86_64-linux-gnu/libvorbis.so.0 \
         /usr/lib/x86_64-linux-gnu/libopenal.so.1 /usr/lib/x86_64-linux-gnu/libfreetype.so.6 \
         /usr/lib/x86_64-linux-gnu/libharfbuzz.so.0 /usr/lib/x86_64-linux-gnu/libjpeg.so.8 \
         /usr/lib/x86_64-linux-gnu/libpng16.so.16 "$@"; do
    f=$(readlink -f "$f" 2>/dev/null) && [ -f "$f" ] && files+=("$f")
done
echo "corpus: ${#files[@]} files"; ls -la "${files[@]}" | awk '{print $5, $NF}'
for f in "${files[@]}"; do
    objdump -d --no-show-raw-insn -M intel "$f" 2>/dev/null > /tmp/corpus_dis.txt
    # pass 1: backward branches -> "target end" (the furthest branch back to each target)
    awk '
    function hex(s,   i, c, v) { v = 0; s = tolower(s); for (i = 1; i <= length(s); i++) { c = index("0123456789abcdef", substr(s, i, 1)); v = v * 16 + c - 1 } return v }
    /^ *[0-9a-f]+:\t/ {
        split($0, f, "\t"); a = $1; sub(/:$/, "", a); ins = f[2]
        if (ins ~ /^j[a-z]+ +[0-9a-f]+ </) { split(ins, w, / +/); t = hex(w[2]); s = hex(a)
            if (t <= s && s - t < 8192 && (!(t in e) || e[t] < s)) e[t] = s }
    }
    END { for (t in e) print t, e[t] }' /tmp/corpus_dis.txt > /tmp/corpus_loops.txt
    echo "@@FILE $(wc -l < /tmp/corpus_loops.txt)"
    cat /tmp/corpus_loops.txt
    echo "@@DIS"
    cat /tmp/corpus_dis.txt
done | awk '
function hex(s,   i, c, v) { v = 0; s = tolower(s); for (i = 1; i <= length(s); i++) { c = index("0123456789abcdef", substr(s, i, 1)); v = v * 16 + c - 1 } return v }
function kind(o) {
    if (o ~ /\[/) return "m"
    if (o ~ /^(0x[0-9a-f]+|-?[0-9]+)$/) return "i"
    if (o ~ /^xmm/) return "x"
    if (o ~ /^r[0-9]+d$|^e[a-z][a-z]$/) return "r32"
    if (o ~ /^r[0-9]+w$|^[a-ds][xip]$|^[sd]il$|^[bs]pl$/) return "r16"
    if (o ~ /^r[0-9]+b$|^[a-d][lh]$|^[sd]il$|^[bs]pl$/) return "r8"
    if (o ~ /^r[a-z0-9]+$/) return "r64"
    return "?"
}
function reg64(o) {   # canonical register for dependency checks ("" for non-registers)
    if (o ~ /\[/ || o ~ /^(0x|-?[0-9])/) return ""
    sub(/^e/, "r", o); sub(/[dwb]$/, "", o); return o
}
/^@@FILE/ { mode = 1; delete endat; active = -1; prev = ""; prev2 = ""; next }
/^@@DIS/ { mode = 2; next }
mode == 1 { endat[$1] = $2; next }
mode == 2 && /^ *[0-9a-f]+:\t/ {
    split($0, f, "\t"); a = $1; sub(/:$/, "", a); ins = f[2]; gsub(/ +$/, "", ins)
    av = hex(a)
    if ((av "") in endat && endat[av ""] > active) active = endat[av ""]
    inloop = av <= active
    if (ins == "" || ins ~ /^(\(bad\)|data16|nop|xchg   ax,ax|int3)/) { prev = ""; prev2 = ""; next }
    n = index(ins, " "); mn = n ? substr(ins, 1, n - 1) : ins; ops = n ? substr(ins, n) : ""
    gsub(/^ +/, "", ops); gsub(/(QWORD|DWORD|WORD|BYTE|XMMWORD) PTR /, "", ops); sub(/ +<.*$/, "", ops)
    if (mn ~ /^(rep|repz|repnz|lock|bnd|notrack|cs|ds)$/) { mn = mn " " ops; ops = "" }
    na = split(ops, w, ","); d = na >= 1 ? w[1] : ""; s = na >= 2 ? w[2] : ""
    shape = mn (na >= 1 ? " " kind(d) : "") (na >= 2 ? "," kind(s) : "") (na >= 3 ? ",i" : "")
    if (mn ~ /^j[a-z]+$/ && mn != "jmp") shape = "jcc"
    if (mn ~ /^set[a-z]+$/) shape = "setcc " kind(d)
    if (mn ~ /^cmov[a-z]+$/) shape = "cmovcc " kind(d) "," kind(s)
    total++; single[shape]++
    if (inloop) { ltotal++; lsingle[shape]++ }
    if (prev != "") {
        rel = ""
        if (pd != "" && reg64(d) == pd && mn !~ /^(push|cmp|test)/) rel = " [same dst]"
        else if (pd != "" && (index(s, pd) || (d ~ /\[/ && index(d, pd)) || (mn ~ /^(cmp|test|push)/ && reg64(d) == pd))) rel = " [uses dst]"
        key = prev " ; " shape rel
        pair[key]++
        if (inloop) { lpair[key]++; if (prev2 != "") ltri[prev2 " ; " key]++ }
        # superinstruction families (FXR candidates), by the shapes involved
        fam = ""
        if (pmn == "mov" && prev ~ /^mov r(32|64),r(32|64)$/ && rel == " [same dst]" && mn ~ /^(add|sub|and|or|xor|shl|shr|sar|rol|ror|imul|neg|not)$/) fam = "mov r,r ; ALU same dst (3-operand)"
        else if (shape == "jcc" && pmn ~ /^(add|sub|and|or|xor|inc|dec|neg|shl|shr|sar)$/ && prev ~ / r(32|64)/) fam = "ALU reg ; jcc"
        else if (shape == "jcc" && pmn ~ /^(cmp|test)$/ && prev ~ /( m,|,m$)/) fam = "cmp/test with memory ; jcc"
        else if (shape == "jcc" && pmn ~ /^(cmp|test)$/ && prev ~ / r(8|16)/) fam = "cmp/test 8/16-bit reg ; jcc"
        else if (shape == "jcc" && pmn ~ /^(cmp|test)$/) fam = "cmp/test 32/64-bit reg ; jcc (fused already)"
        else if (shape == "jcc" && pmn ~ /^v?u?comis[sd]$/) fam = "(u)comis ; jcc"
        else if (pmn == "push" && mn == "push") fam = "push ; push"
        else if (pmn == "pop" && mn == "pop") fam = "pop ; pop"
        else if (pmn == "pop" && mn == "ret") fam = "pop ; ret"
        else if (mn == "call" && (prev ~ /^mov r(32|64),r(32|64)$/ || prev ~ /^lea r64,m$/ || prev ~ /^mov r32,i$/ || prev ~ /^xor r32,r32$/)) fam = "argument setup ; call"
        else if (pmn == "mov" && prev ~ /^mov r(32|64),m$/ && rel == " [uses dst]" && mn ~ /^(test|cmp)$/) fam = "load r ; test/cmp r"
        else if (pmn == "mov" && prev ~ /^mov r(32|64),m$/ && rel == " [same dst]" && mn ~ /^(add|sub|and|or|xor|imul)$/) fam = "load r ; ALU r (same dst)"
        else if (prev ~ /^(add|sub) r(32|64),i$/ && mn == "cmp" && rel == " [uses dst]") fam = "add/sub r,i ; cmp r (loop step)"
        else if (prev ~ /^(inc|dec) r(32|64)$/ && mn == "cmp" && rel == " [uses dst]") fam = "inc/dec r ; cmp r (loop step)"
        else if (prev ~ /^(add|sub) r(32|64),r(32|64)$/ && mn == "cmp" && rel == " [uses dst]") fam = "add/sub r,r ; cmp r (loop step)"
        else if (prev ~ /^movzx r32,m$/ && rel != "") fam = "movzx load ; use"
        if (fam != "") { famc[fam]++; if (inloop) lfamc[fam]++ }
        # single-instruction forms worth a specialised handler
        if (mn ~ /^(add|sub|mul|div|min|max|and|andn|or|xor)(ps|pd|ss|sd)$|^p(add|sub|and|or|xor|cmp)/ && shape ~ /,m$/) { famc["  SSE op with a memory operand"]++; if (inloop) lfamc["  SSE op with a memory operand"]++ }
        if (ops ~ /\[[a-z0-9]+\+[a-z0-9]+\*[1248]/ || ops ~ /\[[a-z0-9]+\+[a-z0-9]+(\+|-|\])/) { famc["  operand with base + index"]++; if (inloop) lfamc["  operand with base + index"]++ }
        # finer splits of the larger families
        sub_ = ""
        if (fam ~ /3-operand/) sub_ = (shape ~ /,i$/ || shape ~ /^(neg|not)/) ? "  3-operand: ALU/shift with an immediate" : "  3-operand: ALU with a register"
        else if (fam ~ /loop step/) sub_ = (shape ~ /,i$/) ? "  loop step: cmp with an immediate" : "  loop step: cmp with a register"
        else if (fam ~ /^ALU reg ; jcc/) sub_ = (prev ~ /,i$/) ? "  ALU reg ; jcc: immediate operand" : (prev ~ /^(inc|dec)/ ? "  ALU reg ; jcc: inc/dec" : "  ALU reg ; jcc: register operand")
        if (sub_ != "") { famc[sub_]++; if (inloop) lfamc[sub_]++ }
    }
    pmn = mn
    if (mn ~ /^(j|call|ret|jmp|ud2|hlt)/) { prev = ""; prev2 = ""; next }
    prev2 = (prev == "" ? "" : prev); prev = shape; pd = (mn ~ /^(push|cmp|test)/) ? "" : reg64(d)
}
END {
    printf "instructions: %d, in loop bodies: %d\n\n", total, ltotal
    print "== superinstruction families: all code / loop bodies (per mille of instructions)"
    for (k in famc) printf "%7.2f %7.2f  %s\n", 1000 * famc[k] / total, 1000 * lfamc[k] / ltotal, k | "sort -rn"
    close("sort -rn")
    print ""
    print "== top single instruction shapes, all code (per mille)"
    for (k in single) printf "%7.2f  %s\n", 1000 * single[k] / total, k | "sort -rn | head -60"
    close("sort -rn | head -60")
    print "\n== top single instruction shapes, loop bodies (per mille of loop-body instructions)"
    for (k in lsingle) printf "%7.2f  %s\n", 1000 * lsingle[k] / ltotal, k | "sort -rn | head -80"
    close("sort -rn | head -80")
    print "\n== top adjacent pairs, all code (per mille)"
    for (k in pair) printf "%7.2f  %s\n", 1000 * pair[k] / total, k | "sort -rn | head -80"
    close("sort -rn | head -80")
    print "\n== top adjacent pairs, loop bodies (per mille of loop-body instructions)"
    for (k in lpair) printf "%7.2f  %s\n", 1000 * lpair[k] / ltotal, k | "sort -rn | head -150"
    close("sort -rn | head -150")
    print "\n== top triples, loop bodies (per mille of loop-body instructions)"
    for (k in ltri) printf "%7.2f  %s\n", 1000 * ltri[k] / ltotal, k | "sort -rn | head -100"
    close("sort -rn | head -100")
}'
