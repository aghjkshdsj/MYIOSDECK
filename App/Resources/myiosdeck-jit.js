// SPDX-License-Identifier: GPL-3.0-or-later
// MYIOSDECK JIT script for StikDebug (iOS 26/27, TXM devices).
//
// MYIOSDECK sends this with stikdebug://enable-jit?...&script-data=<base64>.
// It answers the universal BRK #0xf00d protocol:
//   x16 = 1  prepare region: x0 = address (0 = allocate), x1 = size -> x0 = RX address
//   x16 = 0  detach: the JIT pool stays executable after the debugger leaves
// Signals and faults are handed straight back to the app so its own handlers
// run, and the stub is told not to stop for them at all where it supports it:
// every stop costs StikDebug round-trips, and too many get StikDebug killed by
// iOS's watchdog, which takes the debug session (and the app) down with it.
//
// Derived from Madeira's madeira-jit.js (github.com/willfaust/Madeira,
// GPL-3.0-or-later with the Madeira Converter Exception).

function hexToU64(hex) {
    let n = 0n;
    for (let i = 7; i >= 0; i--) n = (n << 8n) | BigInt(parseInt(hex.substr(i * 2, 2), 16) || 0);
    return n;
}
function u64ToHex(n) {
    let s = '';
    for (let i = 0; i < 8; i++) { s += Number(n & 0xFFn).toString(16).padStart(2, '0'); n >>= 8n; }
    return s;
}
function hexToU32(hex) { return parseInt(hex.match(/../g).reverse().join(''), 16); }
function isStop(r) { return typeof r === 'string' && /^[TSWX]/.test(r); }

const pid = get_pid();
log(`MYIOSDECK JIT: attaching to pid ${pid}`);
log(`MYIOSDECK JIT: ${send_command(`vAttach;${pid.toString(16)}`)}`);

{
    const ign = send_command('QSetIgnoredExceptions:EXC_BAD_ACCESS;EXC_BAD_INSTRUCTION');
    const sigs = [];
    for (let s = 1; s <= 31; s++) if (s !== 5) sigs.push(s.toString(16)); // keep SIGTRAP: BRK arrives that way
    const pass = send_command(`QPassSignals:${sigs.join(';')}`);
    log(`MYIOSDECK JIT: ignore-exceptions=${ign || 'n/a'} pass-signals=${pass || 'n/a'}`);
}

let budget = 30; // each log() is a UI update in StikDebug; keep them rare
function ulog(m) { if (budget > 0) { budget--; log(m); } }

let detached = false, pending = null, lastKey = null, repeats = 0;

function forward(sig, tid) {
    const h = sig.toString(16).padStart(2, '0');
    let r = send_command(`vCont;C${h}:${tid};c`);
    if (!isStop(r)) r = send_command(`C${h}`);
    if (isStop(r)) { pending = r; return true; }
    return false;
}

while (!detached) {
    const stop = pending !== null ? pending : send_command('c');
    pending = null;
    if (typeof stop === 'string' && /^[WX]/.test(stop)) { ulog('MYIOSDECK JIT: app exited'); break; }

    const tid = (/T[0-9a-f]+thread:([0-9a-f]+);/.exec(stop) || [])[1];
    const pcHex = (/20:([0-9a-f]{16});/.exec(stop) || [])[1];
    if (!tid || !pcHex) continue;
    const pc = hexToU64(pcHex);

    const metype = parseInt((/metype:([0-9a-f]+);/.exec(stop) || [0, '0'])[1], 16);
    const medata = [];
    const mre = /medata:([0-9a-fx]+);/g;
    for (let m = mre.exec(stop); m !== null; m = mre.exec(stop)) medata.push(parseInt(m[1], 16));

    if (metype === 5) { // EXC_SOFT_SIGNAL: a real unix signal; deliver the original signo
        const signo = medata.length > 1 && medata[1] >= 1 && medata[1] <= 31 ? medata[1] : 0;
        if (!signo || !forward(signo, tid)) { const r = send_command('c'); if (isStop(r)) pending = r; }
        continue;
    }

    const insn = send_command(`m${pc.toString(16)},4`);
    const ok = typeof insn === 'string' && /^[0-9a-fA-F]{8}$/.test(insn);
    const word = ok ? hexToU32(insn) : 0;
    const isBrk = ok && ((word & 0xFFE0001F) >>> 0) === 0xD4200000;

    if (!isBrk) {
        if (metype === 11) { const r = send_command('c'); if (isStop(r)) pending = r; continue; } // EXC_RESOURCE advisory
        const key = `${tid}:${pcHex}`;
        repeats = key === lastKey ? repeats + 1 : 1;
        lastKey = key;
        const kcode = medata[0] || 0;
        let sig = 11;
        if (metype === 1) sig = kcode === 1 ? 11 : 10;
        else if (metype === 2) sig = 4;
        else if (metype === 3) sig = 8;
        else if (metype === 6) sig = 5;
        ulog(`MYIOSDECK JIT: fault pc=0x${pc.toString(16)} metype=${metype} -> signal ${sig}`);
        if (repeats >= 8) { ulog('MYIOSDECK JIT: fault repeats, stopping the app'); send_command('k'); break; }
        if (!forward(sig, tid)) { const r = send_command('c'); if (isStop(r)) pending = r; }
        continue;
    }

    lastKey = null; repeats = 0;
    const imm = (word >> 5) & 0xFFFF;
    send_command(`P20=${u64ToHex(pc + 4n)};thread:${tid};`); // step past the BRK
    const x16Hex = (/10:([0-9a-f]{16});/.exec(stop) || [])[1];
    if (imm !== 0xf00d || !x16Hex) { send_command(`P0=${u64ToHex(0n)};thread:${tid};`); continue; }

    const x0 = hexToU64((/00:([0-9a-f]{16});/.exec(stop) || [0, '0'.repeat(16)])[1]);
    const x1 = hexToU64((/01:([0-9a-f]{16});/.exec(stop) || [0, '0'.repeat(16)])[1]);
    const cmd = hexToU64(x16Hex);

    if (cmd === 0n) {
        ulog('MYIOSDECK JIT: detach (pool ready)');
        send_command('D');
        detached = true;
    } else if (cmd === 1n) {
        let addr = x0;
        if (addr === 0n && x1 !== 0n) {
            const r = send_command(`_M${x1.toString(16)},rx`);
            if (r && /^[0-9a-fA-F]+$/.test(r)) addr = BigInt(`0x${r}`);
        }
        if (addr !== 0n && x1 !== 0n) prepare_memory_region(addr, x1);
        ulog(`MYIOSDECK JIT: prepared 0x${addr.toString(16)} (${Number(x1 >> 20n)} MB)`);
        send_command(`P0=${u64ToHex(addr)};thread:${tid};`);
    } else {
        send_command(`P0=${u64ToHex(0n)};thread:${tid};`);
    }
}
