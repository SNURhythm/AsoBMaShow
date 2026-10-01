#!/usr/bin/env python3
"""Benchmark old/new parser amalgamations without worktrees or source edits.

Requires the sibling bms-parser-cpp checkout for its fixture corpus. Timings
include parser construction, hashing, chart allocation and destruction. The
file mode additionally includes warm-cache file I/O. No chart assets are read.
"""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import shlex
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COMMANDS = []


def run(args, cwd=ROOT):
    args = list(map(str, args))
    COMMANDS.append(dict(argv=args, cwd=str(cwd)))
    result = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        raise RuntimeError(f'{shlex.join(args)}\n{result.stdout}')
    return result.stdout


def make_corpus(output, parser_repo):
    corpus = output / 'corpus'
    corpus.mkdir(exist_ok=True)
    cases = []
    def add(name, data, source):
        path = corpus / (name + '.bms')
        path.write_bytes(data)
        cases.append(dict(name=name, path=str(path), bytes=len(data), source=source,
                          sha256=hashlib.sha256(data).hexdigest()))
    for path in sorted((parser_repo / 'test/testcases').rglob('*')):
        if path.suffix.lower() in ('.bms', '.bme', '.bml', '.pms'):
            name = 'fixture_' + path.stem
            add(name, path.read_bytes(), str(path.relative_to(parser_repo)))
    for path in sorted((ROOT / 'tests/fixtures').rglob('*.bms')):
        add('game_' + path.stem, path.read_bytes(), str(path.relative_to(ROOT)))
    header = '#PLAYER 1\n#TITLE Benchmark\n#ARTIST Test\n#BPM 150\n#RANK 2\n#WAV01 note.wav\n'
    sparse = ''.join(f'#{measure:03d}11:01000000\n' for measure in range(160))
    dense = ''.join(f'#{measure:03d}{lane}:'+('0100010001000000'*8)+'\n'
                    for measure in range(160) for lane in ('11','12','13','14','15','18','19','01'))
    add('synthetic_ascii_sparse', (header+sparse).encode(), 'generated: sparse ASCII')
    add('synthetic_ascii_dense', (header+dense).encode(), 'generated: dense ASCII')
    titles = [('utf8', '별빛 아래에서 青空の向こう側 😀', 'utf-8'),
              ('shiftjis', '青空の向こう側', 'shift_jis'),
              ('euckr', '별빛 아래에서', 'euc_kr')]
    for name, title, encoding in titles:
        charset = {'utf8':'UTF-8', 'shiftjis':'SHIFT_JIS', 'euckr':'EUC-KR'}[name]
        value = f'#CHARSET {charset}\n' + header.replace('Benchmark', title) + dense
        add('synthetic_'+name, value.encode(encoding), 'generated: declared '+encoding)
    # Exercise the optimized whole-file declaration scan and automatic detector,
    # including Shift-JIS's special backslash/tilde mapping in resource names.
    sjis = header.replace('Benchmark', '青空').replace('note.wav', 'sound\\note~.wav')
    add('synthetic_sjis_auto', (sjis+dense).encode('shift_jis'), 'generated: automatic Shift-JIS')
    add('synthetic_late_charset', (sjis+dense+'#CHARSET SHIFT_JIS').encode('shift_jis'),
        'generated: late declaration, unterminated final line')
    if len({case['name'] for case in cases}) != len(cases):
        raise RuntimeError('Duplicate corpus names')
    return cases


def build(options, output):
    parser_repo = options.parser_repo.resolve()
    baseline = run(['git', 'rev-parse', options.baseline]).strip()
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    hashes = {}
    for backend in ('baseline', 'candidate'):
        destination = output / backend
        destination.mkdir(exist_ok=True)
        for name in ('bms_parser.hpp', 'bms_parser.cpp'):
            if backend == 'baseline':
                data = subprocess.check_output(['git', 'show', f'{baseline}:src/{name}'], cwd=ROOT)
            else:
                data = (ROOT / 'src' / name).read_bytes()
            (destination / name).write_bytes(data)
            hashes[f'{backend}/{name}'] = hashlib.sha256(data).hexdigest()
        run([*compiler, '-std=c++23', '-O3', '-g0', '-DNDEBUG', '-DBMS_PARSER_VERBOSE=0',
             '-pthread', f'-I{destination}', ROOT / 'tests/benchmarks/parser_benchmark.cpp',
             destination / 'bms_parser.cpp', '-o', destination / 'benchmark'])
    cases = make_corpus(output, parser_repo)
    manifest = dict(baseline=baseline, parser_revision=run(['git','rev-parse','HEAD'], parser_repo).strip(),
                    compiler=run([*compiler,'--version']).strip(), platform=platform.platform(),
                    source_sha256=hashes, corpus=cases, commands=COMMANDS,
                    binary_sha256={b:hashlib.sha256((output/b/'benchmark').read_bytes()).hexdigest()
                                   for b in ('baseline','candidate')},
                    harness_sha256=hashlib.sha256((ROOT/'tests/benchmarks/parser_benchmark.cpp').read_bytes()).hexdigest())
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    return manifest


def measure(options, output, manifest):
    cases = manifest['corpus']
    for backend, digest in manifest['binary_sha256'].items():
        if hashlib.sha256((output/backend/'benchmark').read_bytes()).hexdigest() != digest:
            raise RuntimeError(f'Benchmark binary changed: {backend}')
    for case in cases:
        if hashlib.sha256(Path(case['path']).read_bytes()).hexdigest() != case['sha256']:
            raise RuntimeError(f"Corpus file changed: {case['name']}")
    verification = []
    for case in cases:
        for mode in ('full', 'metadata', 'file', 'ready'):
            values = {backend: run([output/backend/'benchmark', case['path'], mode, 1, 'verify']).strip()
                      for backend in ('baseline','candidate')}
            if values['baseline'] != values['candidate']:
                raise RuntimeError(f"Semantic mismatch: {case['name']}/{mode}: {values}")
            verification.append(dict(case=case['name'], mode=mode, result=values['candidate']))
    print(f'Equivalent chart digests in {len(verification)} corpus/mode comparisons', flush=True)
    if options.verify_only:
        return
    rows = []
    rng = random.Random(20261001)
    for sample in range(options.samples):
        tasks = [(case, mode) for case in cases for mode in ('full','metadata','file')]
        rng.shuffle(tasks)
        for case, mode in tasks:
            iterations = max(3, min(100, 1024*1024 // max(1,case['bytes'])))
            order = ['baseline','candidate']; rng.shuffle(order)
            checksums = []
            for backend in order:
                log = run([output/backend/'benchmark', case['path'], mode, iterations, 'time'])
                match = re.fullmatch(r'us=([\d.e+\-]+) checksum=(\d+)\n', log)
                if not match: raise RuntimeError(log)
                micros, checksum = match.groups(); checksums.append(checksum)
                rows.append(dict(sample=sample, case=case['name'], mode=mode, backend=backend,
                                 bytes=case['bytes'], iterations=iterations, us=float(micros), checksum=checksum))
            if checksums[0] != checksums[1]: raise RuntimeError('Timed output mismatch')
        print(f'Measured sample {sample+1}/{options.samples}', flush=True)
    summaries = []
    for case in cases:
        for mode in ('full','metadata','file'):
            group = [r for r in rows if r['case']==case['name'] and r['mode']==mode]
            summary = dict(case=case['name'], mode=mode, bytes=case['bytes'])
            for backend in ('baseline','candidate'):
                values = [r['us'] for r in group if r['backend']==backend]
                summary[backend] = dict(median=statistics.median(values), minimum=min(values), maximum=max(values))
            summary['reduction_percent'] = 100*(1-summary['candidate']['median']/summary['baseline']['median'])
            summaries.append(summary)
    result = dict(manifest=manifest, samples=options.samples, verification=verification, rows=rows, summary=summaries)
    (output/'results.json').write_text(json.dumps(result, indent=2)+'\n')
    with (output/'samples.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator='\n')
        writer.writeheader(); writer.writerows(rows)
    print(f"Results: {output/'results.json'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', default='486e375d')
    parser.add_argument('--parser-repo', type=Path, default=ROOT.parent/'bms-parser-cpp')
    parser.add_argument('--output', type=Path, default=Path('/tmp/asobmashow-parser-adoption'))
    parser.add_argument('--samples', type=int, default=7)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--skip-build', action='store_true')
    options = parser.parse_args()
    output = options.output.resolve()
    if options.samples < 1 or output == ROOT or ROOT in output.parents:
        parser.error('samples must be positive and output must be outside the checkout')
    output.mkdir(parents=True, exist_ok=True)
    manifest = (json.loads((output/'manifest.json').read_text()) if options.skip_build else build(options, output))
    if not options.build_only: measure(options, output, manifest)


if __name__ == '__main__':
    main()
