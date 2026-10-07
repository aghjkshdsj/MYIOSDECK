# SPDX-License-Identifier: GPL-3.0-or-later
"""Three paired, rotating-order trials, guest clock only, exact checksums."""
import json
import os
import pathlib
import platform
import re
import statistics
import subprocess
import sys

guests, astra, fxi, native, output = sys.argv[1:]
output = pathlib.Path(output)
output.mkdir(parents=True, exist_ok=True)
arm = platform.machine() == "aarch64"
specs = {"integer": 3, "float": 16, "memory": 50, "branch": 6, "simd": 150}
data = {"architecture": platform.machine(), "trials": {}, "errors": []}

def run(cmd):
    p = subprocess.run(cmd, text=True, capture_output=True, timeout=240)
    if p.returncode:
        raise RuntimeError(f"{cmd}: exit={p.returncode}\n{p.stdout}\n{p.stderr}")
    return p.stdout

hello_guest = str(pathlib.Path(guests) / "hello.elf")
try:
    # hello contains a timer and host-dependent CPUID strings. Preserve all raw
    # outputs; compare deterministic prose and validate the variable fields.
    reference = (pathlib.Path(guests) / "hello-native.txt").read_text()
    def normalized(s):
        s = re.sub(r"10M-iteration loop: [0-9]+ us", "10M-iteration loop: <time> us", s)
        s = re.sub(r"Guest CPU vendor: [^\n]*", "Guest CPU vendor: <vendor>", s)
        s = re.sub(r"Guest CPU brand:  [^\n]*", "Guest CPU brand:  <brand>", s)
        return re.sub(r"SSE2 (yes|no) \| SSE4\.2 (yes|no) \| AVX (yes|no)", "<features>", s)
    for engine, binary in (("astra", astra), ("fxi", fxi)):
        value = run([binary, hello_guest])
        (output / f"hello-{engine}.txt").write_text(value)
        if normalized(value) != normalized(reference):
            raise RuntimeError(f"{engine} hello deterministic output mismatch")
        if not re.search(r"10M-iteration loop: [0-9]+ us", value):
            raise RuntimeError(f"{engine} hello missing timed loop")
    data["hello_normalized_match"] = True
    data["hello_literal_match"] = False
except Exception as e:
    data["errors"].append(str(e))

for kernel, scale in specs.items():
    rows = {engine: [] for engine in ("native", "astra", "fxi")}
    checksums = set()
    for trial in range(3):
        engines = list(rows)
        engines = engines[trial:] + engines[:trial]
        for engine in engines:
            guest = str(pathlib.Path(guests) / "bench_sse2.elf")
            cmd = ([native] if arm else [guest]) if engine == "native" else [astra if engine == "astra" else fxi, guest]
            try:
                value = run(cmd + [kernel, str(scale)])
                (output / f"{kernel}-{engine}-{trial}.txt").write_text(value)
                match = re.fullmatch(r"RESULT kernel=(\w+) ns=(\d+) sum=(\d+)\s*", value)
                if not match or match[1] != kernel or int(match[2]) <= 0:
                    raise RuntimeError(f"invalid RESULT: {value!r}")
                rows[engine].append(int(match[2]))
                checksums.add(match[3])
            except Exception as e:
                data["errors"].append(str(e))
    if len(checksums) != 1:
        data["errors"].append(f"{kernel}: checksum mismatch {sorted(checksums)}")
    data["trials"][kernel] = {"ns": rows, "checksums": sorted(checksums), "scale": scale}
    print(kernel, data["trials"][kernel], flush=True)

lines = [f"## Astra vs FXI — {platform.machine()} (median of 3)", "",
         "| Kernel | Scale | Native ns | Astra ns | Astra % | FXI ns | FXI % | Checksum |",
         "|---|---:|---:|---:|---:|---:|---:|---|"]
percentages = {"astra": [], "fxi": []}
for kernel, row in data["trials"].items():
    if not all(len(v) == 3 for v in row["ns"].values()):
        lines.append(f"| {kernel} | {row['scale']} | incomplete | failed | — | — | — | — |")
        continue
    med = {k: statistics.median(v) for k, v in row["ns"].items()}
    pct = {k: 100 * med["native"] / med[k] for k in percentages}
    for k in pct:
        percentages[k].append(pct[k])
    row.update(median_ns=med, speed_percent=pct)
    checksum = row["checksums"][0] if len(row["checksums"]) == 1 else "MISMATCH"
    lines.append(f"| {kernel} | {row['scale']} | {med['native']} | {med['astra']} | {pct['astra']:.2f}% | {med['fxi']} | {pct['fxi']:.2f}% | {checksum} |")
means = {k: statistics.mean(v) if len(v) == 5 else None for k, v in percentages.items()}
data["mean_percent"] = means
data["performance_gate"] = arm and not data["errors"] and means["astra"] is not None and means["astra"] >= 12
lines += ["", f"Arithmetic means: {means}", "",
          f"ARM64 performance gate (normalized hello): **{data['performance_gate']}**.",
          "Hello comparison normalizes elapsed time and CPUID identity/features; raw output is archived. Literal equality is not claimed."]
if data["errors"]:
    lines += ["", "Errors:", "```", *data["errors"], "```"]
summary = "\n".join(lines) + "\n"
print(summary)
(output / "results.json").write_text(json.dumps(data, indent=2))
(output / "summary.md").write_text(summary)
if os.environ.get("GITHUB_STEP_SUMMARY"):
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
        f.write(summary)
sys.exit(bool(data["errors"]) or (arm and not data["performance_gate"]))
