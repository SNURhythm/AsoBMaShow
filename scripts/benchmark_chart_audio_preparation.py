#!/usr/bin/env python3
"""Measure production chart audio preparation and preview-to-gameplay handoff.

Uses a headless backend and real loaders with the existing test dependency closure. Builds
optimized objects outside the checkout and links installed release dependencies.
Charts and assets are read in place; paths and raw logs remain in the output dir.
No real audio device, visual asset loading, or app UI is involved.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--charts-from', type=Path, required=True, help='One chart path per line')
parser.add_argument('--build-dir', type=Path, default=root / 'cmake-build-debug')
parser.add_argument('--output', type=Path)
parser.add_argument('--rounds', type=int, default=3)
parser.add_argument('--cache-bytes', type=int, help='Probe a retention budget; default is unlimited')
parser.add_argument('--sample-rates', type=int, nargs='+', default=[44100, 48000], choices=[44100, 48000])
parser.add_argument('--sample-seconds', type=int, default=0,
                    help='Optional macOS sample duration per rate; use a separate profiling run')
options = parser.parse_args()
if not 1 <= options.rounds <= 100 or not 0 <= options.sample_seconds <= 30:
    parser.error('rounds must be 1..100 and sample-seconds 0..30')
build = options.build_dir.resolve()
out = (options.output or Path(tempfile.mkdtemp(prefix='asobmashow-chart-audio-'))).resolve()
if out.is_relative_to(root) or out == build:
    parser.error('--output must be outside the checkout/build directory')
charts = [str(Path(line).expanduser().resolve())
          for line in options.charts_from.read_text().splitlines() if line]
if options.cache_bytes is not None and options.cache_bytes < 0:
    parser.error('--cache-bytes must be nonnegative')
if not charts:
    parser.error('--charts-from is empty')
out.mkdir(parents=True, exist_ok=True)
commands = json.loads((build / 'compile_commands.json').read_text())
target = 'jukebox_restore_tests'
selected = {entry['file']: entry for entry in commands
            if f'CMakeFiles/{target}.dir/' in entry['command']}
if not selected:
    raise RuntimeError('Configure/build jukebox_restore_tests first')
# Preview's existing target supplies its source/dependency closure and flags.
for entry in commands:
    if 'CMakeFiles/paged_preview_source_tests.dir/' in entry['command'] and '/tests/' not in entry['file']:
        selected.setdefault(entry['file'], entry)
objects, extras, jobs = {}, [], []
for entry in selected.values():
    args = shlex.split(entry['command'])
    if Path(entry['file']) == root / 'tests/jukebox_restore_tests.cpp':
        entry['file'] = str(root / 'tests/chart_audio_preparation_benchmark.cpp')
        args[args.index('-c') + 1] = entry['file']
    original = args[args.index('-o') + 1]
    destination = (out / original).resolve()
    if not destination.is_relative_to(out):
        raise ValueError('Object path escapes benchmark directory')
    destination.parent.mkdir(parents=True, exist_ok=True)
    objects[original] = str(destination)
    if f'CMakeFiles/{target}.dir/' not in original:
        extras.append(str(destination))
    args[args.index('-o') + 1] = str(destination)
    args = [arg for arg in args if arg not in
            ('-g', '-g1', '-g0', '-gline-tables-only', '-O0', '-O1', '-O2', '-O3', '-Os', '-Oz', '-Ofast')]
    args.extend(['-O2', '-g1'])
    jobs.append((args, entry['directory']))


def compile_source(job):
    args, cwd = job
    result = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(result.stdout)
    print('Compiled', Path(args[args.index('-c') + 1]).name, flush=True)


with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    list(pool.map(compile_source, jobs))
cache = (build / 'CMakeCache.txt').read_text().splitlines()
ninja = next(line.split('=', 1)[1] for line in cache if line.startswith('CMAKE_MAKE_PROGRAM:FILEPATH='))
link = subprocess.check_output([ninja, '-C', str(build), '-t', 'commands', target], text=True).splitlines()[-1]
tokens = shlex.split(link)
if tokens[:2] != [':', '&&'] or tokens[-2:] != ['&&', ':'] or tokens.count('&&') != 2:
    raise RuntimeError('Expected the CMake Unix Ninja link layout')
args = [objects.get(arg, arg) for arg in tokens[2:-2]]
release_names = {'libcurl-d.a': 'libcurl.a', 'liblz4d.a': 'liblz4.a', 'libbz2d.a': 'libbz2.a'}
for index, arg in enumerate(args):
    if '/debug/lib/' in arg and arg.startswith('-'):
        args[index] = arg.replace('/debug/lib/', '/lib/')
    elif '/debug/lib/' in arg:
        release = Path(arg.replace('/debug/lib/', '/lib/'))
        release = release.with_name(release_names.get(release.name, release.name))
        if not (build / release).is_file():
            raise RuntimeError('Missing release dependency: ' + str(release))
        args[index] = str(release)
args[1:1] = extras
args[args.index('-o') + 1] = str(out / 'benchmark')
subprocess.run(args, cwd=build, check=True)
manifest = {
    'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
    'charts': charts, 'rounds': options.rounds, 'sample_rates': options.sample_rates,
    'sample_seconds': options.sample_seconds, 'dependency_build': str(build),
    'cache_bytes': options.cache_bytes,
    'sources': {entry['file']: hashlib.sha256(Path(entry['file']).read_bytes()).hexdigest()
                for entry in selected.values()},
    'compile_commands': [args for args, _ in jobs], 'link_command': args,
}
(out / 'commands.json').write_text(json.dumps(manifest, indent=2) + '\n')
environment = os.environ.copy()
environment.pop('ASOBMASHOW_BENCHMARK_CACHE_BYTES', None)
if options.cache_bytes is not None:
    environment['ASOBMASHOW_BENCHMARK_CACHE_BYTES'] = str(options.cache_bytes)
for rate in options.sample_rates:
    with (out / f'benchmark-{rate}.log').open('w') as log, \
         (out / f'benchmark-{rate}.stderr.log').open('w') as errors:
        process = subprocess.Popen([str(out / 'benchmark'), '--benchmark-chart-audio',
                                    str(rate), str(options.rounds), *charts],
                                   cwd=root, stdout=log, stderr=errors, env=environment)
        sampler = None
        if options.sample_seconds:
            sampler = subprocess.Popen(['sample', str(process.pid), str(options.sample_seconds),
                                        '-file', str(out / f'sample-{rate}.txt')],
                                       stdout=errors, stderr=errors)
        result = process.wait()
        if sampler is not None:
            sample_result = sampler.wait()
            if sample_result:
                print('Sampling failed; inspect stderr log for', rate, flush=True)
        if result:
            raise RuntimeError(f'Benchmark failed ({result}); inspect {out}/benchmark-{rate}.stderr.log')
    print('Benchmark results:', out / f'benchmark-{rate}.log', flush=True)
