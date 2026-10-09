#!/usr/bin/env python3
"""Collect serialized, balanced A/B runs; compilation must finish beforehand."""
import argparse
import hashlib
import json
import math
import random
import statistics
import subprocess
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--pairs', type=int, default=20)
args = parser.parse_args()
if args.pairs < 8 or args.pairs % 2:
    parser.error('--pairs must be an even number >= 8')
repo = Path(__file__).resolve().parents[4]
font = repo / 'assets/fonts/notosanscjkjp.ttf'
seed = 20261009

def command(*items):
    return subprocess.check_output(items, cwd=repo, text=True).strip()

def sample(version):
    start = time.monotonic()
    result = subprocess.run([str(args.build.resolve() / f'probe{version}'), str(font)],
                            cwd=repo, text=True, capture_output=True, check=True)
    data = json.loads(result.stdout)
    assert data['sdl_major'] == version and data['checksum'] > 0
    assert len(data['metrics']) == 15
    assert len({m['name'] for m in data['metrics']}) == 15
    assert all(m['operations'] > 0 and math.isfinite(m['ns_per_op']) and
               m['ns_per_op'] > 0 for m in data['metrics'])
    data['process_seconds'] = time.monotonic() - start
    return data

metadata = {
    'baseline': command('git', 'rev-parse', '1aa029e5'),
    'candidate': command('git', 'rev-parse', 'ef61b16f'),
    'sdl3_commit': command('git', '-C', str(repo/'SDL'), 'rev-parse', 'HEAD'),
    'ttf3_commit': command('git', '-C', str(repo/'SDL_ttf'), 'rev-parse', 'HEAD'),
    'compiler': command('/usr/bin/c++', '--version'),
    'cpu': command('sysctl', '-n', 'machdep.cpu.brand_string'),
    'memory_bytes': int(command('sysctl', '-n', 'hw.memsize')),
    'os': command('sw_vers', '-productVersion'),
    'font_sha256': hashlib.sha256(font.read_bytes()).hexdigest(),
    'harness_sha256': hashlib.sha256(Path(__file__).with_name('probe.cpp').read_bytes()).hexdigest(),
    'seed': seed, 'pairs': args.pairs, 'build_type': 'Release (-O3 -DNDEBUG)',
    'units': 'nanoseconds per operation',
    'method': 'Balanced shuffled AB/BA order; one excluded process warmup per version; '
              'each metric also has one excluded full-batch warmup; all retained runs kept.',
}
# These complete-process calibration runs are excluded by design.
sample(2)
sample(3)
orders = [(2, 3)] * (args.pairs // 2) + [(3, 2)] * (args.pairs // 2)
random.Random(seed).shuffle(orders)
pairs = []
for index, order in enumerate(orders):
    records = {str(version): sample(version) for version in order}
    assert [(m['name'], m['operations']) for m in records['2']['metrics']] == [
        (m['name'], m['operations']) for m in records['3']['metrics']]
    pairs.append({'pair': index, 'order': order, 'runs': records})
    args.output.write_text(json.dumps({'metadata': metadata, 'pairs': pairs}, indent=2)+'\n')
    if (index+1) % 5 == 0:
        print(f'Collected {index+1}/{args.pairs} pairs', flush=True)

# Paired bootstrap: resample whole A/B pairs, preserving within-pair matching.
rng = random.Random(seed)
indices = [[rng.randrange(args.pairs) for _ in pairs] for _ in range(20000)]
summary = []
for position, descriptor in enumerate(pairs[0]['runs']['2']['metrics']):
    before = [p['runs']['2']['metrics'][position]['ns_per_op'] for p in pairs]
    after = [p['runs']['3']['metrics'][position]['ns_per_op'] for p in pairs]
    base, candidate = statistics.median(before), statistics.median(after)
    deltas = sorted((statistics.median(after[i] for i in draws) /
                     statistics.median(before[i] for i in draws)-1)*100 for draws in indices)
    lo, hi = deltas[499], deltas[19499]
    summary.append({'metric': descriptor['name'], 'baseline_ns': base,
                    'candidate_ns': candidate, 'change_percent': (candidate/base-1)*100,
                    'paired_bootstrap_95_percent': [lo,hi],
                    'baseline_min_max_ns': [min(before),max(before)],
                    'candidate_min_max_ns': [min(after),max(after)]})
args.output.with_name('summary.json').write_text(json.dumps(summary, indent=2)+'\n')
for row in summary:
    lo,hi = row['paired_bootstrap_95_percent']
    print(f"{row['metric']:30} {row['baseline_ns']/1000:9.3f} -> "
          f"{row['candidate_ns']/1000:9.3f} us  {row['change_percent']:+7.1f}% "
          f"95% [{lo:+.1f}, {hi:+.1f}]")
