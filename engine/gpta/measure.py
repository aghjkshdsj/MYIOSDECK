#!/usr/bin/env python3
"""Evaluation only; never imported by the interpreter. Exact ratios, five samples."""
import json
import os
import pathlib
import re
import statistics
import subprocess
import sys
from fractions import Fraction

guest, gpta, fxi, native, heldout, *count = sys.argv[1:]
assert not count or count == ['5'], 'Exactly five samples are required'
engines = ('native', 'gpta', 'fxi')
allowed = sorted(os.sched_getaffinity(0))
core = 1 if 1 in allowed else allowed[0]
pin = ['taskset', '-c', str(core)]
suites = (
    ('standard', 'bench_sse2.elf', native,
     'integer:3 float:16 memory:50 branch:6 simd:150'),
    ('held-out', 'heldout.elf', heldout,
     'crc32:75 sort:5 hash:10 vm:15 sha256:40 lz77:100 nbody:50 huffman:65 search:100 tree:3 particles:100'),
)
report = {'commit': os.environ.get('GITHUB_SHA'), 'core': core, 'samples': [], 'rows': []}
destination = pathlib.Path(os.environ.get('GPTA_RESULTS', 'gpta-results.json'))

def display(value):
    # Truncate, never round up. All comparisons/means use the untruncated fraction.
    hundredths = (value.numerator * 100) // value.denominator
    return f'{hundredths // 100}.{hundredths % 100:02}'

def emit(line=''):
    print(line, flush=True)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as f:
            f.write(line + '\n')

try:
    for suite, elf, driver, specs in suites:
        emit(f'### {suite}: median of 5, rotating order, core {core}')
        emit()
        emit('| kernel | native ns | GPTA ns | GPTA % | FXI ns | FXI % |')
        emit('|---|---:|---:|---:|---:|---:|')
        ratios = {e: [] for e in ('gpta', 'fxi')}
        for spec in specs.split():
            kernel, scale = spec.split(':')
            timings = {e: [] for e in engines}
            checksums = set()
            for repetition in range(5):
                for index in range(3):
                    engine = engines[(repetition + index) % 3]
                    executable = str(pathlib.Path(guest) / elf)
                    command = ([executable if driver == 'elf' else driver] if engine == 'native'
                               else [gpta if engine == 'gpta' else fxi, executable])
                    command = pin + command + [kernel, scale]
                    p = subprocess.run(command, capture_output=True, text=True, timeout=600)
                    sample = dict(suite=suite, kernel=kernel, scale=int(scale),
                                  repetition=repetition, order=index, engine=engine,
                                  command=command, returncode=p.returncode,
                                  stdout=p.stdout, stderr=p.stderr)
                    report['samples'].append(sample)
                    assert p.returncode == 0, sample
                    result = re.findall(r'^RESULT kernel=(\w+) ns=(\d+) sum=(\d+)\s*$', p.stdout, re.M)
                    assert len(result) == 1 and result[0][0] == kernel, sample
                    ns, checksum = map(int, result[0][1:])
                    assert ns > 0, sample
                    timings[engine].append(ns)
                    checksums.add(checksum)
                    print(f'SAMPLE {suite} {kernel} round={repetition} order={index} '
                          f'engine={engine} ns={ns} sum={checksum} exit=0', flush=True)
            assert len(checksums) == 1, (suite, kernel, checksums)
            medians = {e: statistics.median(v) for e, v in timings.items()}
            percentages = {e: Fraction(100 * medians['native'], medians[e]) for e in ratios}
            for e in ratios:
                ratios[e].append(percentages[e])
            row = dict(suite=suite, kernel=kernel, medians=medians,
                       checksums=list(checksums), raw=timings,
                       percentages={e: str(p) for e, p in percentages.items()})
            report['rows'].append(row)
            emit(f'| {kernel} | {medians["native"]} | {medians["gpta"]} | '
                 f'{display(percentages["gpta"])}% | {medians["fxi"]} | {display(percentages["fxi"])}% |')
            print('ROW ' + json.dumps(row), flush=True)
        means = {e: sum(v) / len(v) for e, v in ratios.items()}
        report[suite] = {e: str(v) for e, v in means.items()}
        emit(f'MEAN {suite}: GPTA {display(means["gpta"])}% | FXI {display(means["fxi"])}%')
        emit('50% reached on this set.' if means['gpta'] >= 50 else 'Below 50% on this set.')
finally:
    destination.write_text(json.dumps(report, indent=2) + '\n')
