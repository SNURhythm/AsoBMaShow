#!/usr/bin/env python3
"""Compare SQLite mmap using optimized selector queries and existing build deps.

Requires a configured Ninja build with chart_selector_query_tests already built.
Recompiles that target's translation units with -O2 and assertions enabled in a
separate output directory, reusing its dependency libraries without changing the
CMake build. Run with no other builds/tests active for useful timings.
"""

import argparse
import concurrent.futures
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, default=root / 'cmake-build-debug')
parser.add_argument('--output', type=Path)
options = parser.parse_args()
build = options.build_dir.resolve()
out = (options.output or Path(tempfile.mkdtemp(prefix='asobmashow-mmap-'))).resolve()
if out == build:
    raise ValueError('--output must differ from --build-dir to preserve its objects')
out.mkdir(parents=True, exist_ok=True)
commands = [c for c in json.loads((build / 'compile_commands.json').read_text())
            if 'CMakeFiles/chart_selector_query_tests.dir/' in c['command']]
if not commands:
    raise RuntimeError('Build chart_selector_query_tests in the configured Ninja build first')
objects = {}
jobs = []
for entry in commands:
    args = shlex.split(entry['command'])
    index = args.index('-o') + 1
    original = args[index]
    destination = (out / original).resolve()
    if not destination.is_relative_to(out):
        raise ValueError('Compile command object path escapes the output directory')
    destination.parent.mkdir(parents=True, exist_ok=True)
    objects[original] = str(destination)
    args[index] = str(destination)
    args = [a for a in args if a not in
            ('-g', '-O0', '-O1', '-O2', '-O3', '-Os', '-Oz', '-Ofast')]
    args.extend(['-O2', '-g0'])
    jobs.append((args, entry['directory']))

def compile(job):
    args, cwd = job
    result = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(result.stdout)
    print('Compiled', args[args.index('-c')+1], flush=True)

with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    list(pool.map(compile, jobs))
# Use the configured Ninja executable, matching the build's dependency database.
cache = (build / 'CMakeCache.txt').read_text().splitlines()
ninja = next(line.split('=', 1)[1] for line in cache
             if line.startswith('CMAKE_MAKE_PROGRAM:FILEPATH='))
link = subprocess.check_output([ninja, '-C', str(build), '-t', 'commands',
                               'chart_selector_query_tests'], text=True).splitlines()[-1]
tokens = shlex.split(link)
if tokens[:2] != [':', '&&'] or tokens[-2:] != ['&&', ':'] or tokens.count('&&') != 2:
    raise RuntimeError('Unsupported link command; expected the CMake Unix Ninja layout')
args = tokens[2:-2]
args = [objects.get(arg, arg) for arg in args]
args[args.index('-o')+1] = str(out / 'benchmark')
subprocess.run(args, cwd=build, check=True)
manifest = {'compile_commands': [args for args, _ in jobs],
            'link_command': args, 'dependency_build': str(build)}
(out / 'commands.json').write_text(json.dumps(manifest, indent=2) + '\n')
log = out / 'benchmark.log'
with log.open('w') as stream:
    subprocess.run([str(out / 'benchmark'), '--benchmark-mmap'], cwd=root,
                   stdout=stream, stderr=subprocess.STDOUT, check=True)
print('Benchmark results:', log, flush=True)
