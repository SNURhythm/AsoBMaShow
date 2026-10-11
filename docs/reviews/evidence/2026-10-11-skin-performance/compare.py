#!/usr/bin/env python3
"""Measure both binaries sequentially; never run alongside builds/tests."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--skin-root', type=Path, required=True)
parser.add_argument('--samples', type=int, default=9)
parser.add_argument('--frames', type=int, default=20000)
options = parser.parse_args()
if options.samples < 3 or options.frames < 1:
    parser.error('need at least three loading samples and one frame')
work = options.output.resolve()
cases = [
    ('litone-gameplay', 'LITONE12', 'Play/play7.luaskin'),
    ('litone-select', 'LITONE12', 'Select/select.luaskin'),
    ('modern-gameplay', 'ModernChic', 'play7_hw.luaskin'),
    ('modern-select', 'ModernChic', 'musicselect.luaskin'),
]
records = []
for index, (name, skin, entry) in enumerate(cases):
    for variant in (('before', 'after') if index % 2 == 0 else ('after', 'before')):
        command = [str(work / f'probe-{variant}'), '--benchmark',
                   '--samples', str(options.samples), '--frame-samples', str(options.frames),
                   '--skin', str(options.skin_root / skin), '--entry', entry]
        start = time.monotonic()
        result = subprocess.run(command, capture_output=True, text=True)
        (work / f'{name}-{variant}.stderr').write_text(result.stderr)
        if result.returncode:
            raise RuntimeError(f'{name} {variant}: {result.stdout}\n{result.stderr}')
        data = json.loads(result.stdout)
        record = dict(case=name, variant=variant, command=command,
                      wallSeconds=time.monotonic() - start, result=data)
        records.append(record)
        (work / 'comparison.json').write_text(json.dumps(records, indent=2) + '\n')
        frames = data['frames']['frameEvaluation']
        print(name, variant,
              'cold_ms', statistics.median(data['coldSamplesMicros']) / 1000,
              'warm_ms', statistics.median(data['warmSamplesMicros'][1:]) / 1000,
              'frame_us', frames['medianNanos'] / 1000, flush=True)

summary = []
for name, _, _ in cases:
    before, after = (next(row['result'] for row in records
                         if row['case'] == name and row['variant'] == variant)
                     for variant in ('before', 'after'))
    a, b = before['frames'], after['frames']
    assert a['diagnosticCodes'] == b['diagnosticCodes'], name
    if name.endswith('-select'):
        for key in ('batches', 'vertices', 'geometryDigest'):
            assert a['frameEvaluation'][key] == b['frameEvaluation'][key], (name, key)
    else:
        assert a['frameEvaluation']['commands'] == b['frameEvaluation']['commands'], name
        assert not a['callbackBudget']['violationDiagnostics'], name
        assert not b['callbackBudget']['violationDiagnostics'], name
    for metric in ('cold', 'warm', 'frameMedian', 'frameP95'):
        def value(result):
            if metric in ('cold', 'warm'):
                samples = result[f'{metric}SamplesMicros']
                if metric == 'warm': samples = samples[1:]
                return statistics.median(samples) / 1000
            key = 'medianNanos' if metric == 'frameMedian' else 'p95Nanos'
            return result['frames']['frameEvaluation'][key] / 1e6
        old, new = value(before), value(after)
        summary.append(dict(case=name, metric=metric, beforeMillis=old,
                            afterMillis=new, reductionPercent=(old-new)*100/old))
(work / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
