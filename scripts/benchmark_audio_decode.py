#!/usr/bin/env python3
"""Benchmark actual baseline/current audio decoders with private local audio.

Requires an existing Unix Ninja build of skin_sound_bundle_decode_tests. Builds
optimized objects separately, reuses dependency libraries, and keeps all audio
paths, source snapshots, commands, and results in the chosen output directory.
Allocation counters cover ordinary C++ new/new[], not codec-internal malloc.
"""

import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', required=True, help='Git revision before the decoder change')
parser.add_argument('--files-from', type=Path, required=True, help='One local audio path per line')
parser.add_argument('--build-dir', type=Path, default=root / 'cmake-build-debug')
parser.add_argument('--output', type=Path)
options = parser.parse_args()
build = options.build_dir.resolve()
out = (options.output or Path(tempfile.mkdtemp(prefix='asobmashow-audio-'))).resolve()
if out == build or out.is_relative_to(root):
    raise ValueError('--output must be outside the repository and dependency build')
files = [str(Path(line).expanduser().resolve())
         for line in options.files_from.read_text().splitlines() if line]
if not files or any(not Path(path).is_file() for path in files):
    raise ValueError('--files-from must contain existing audio files')
out.mkdir(parents=True, exist_ok=True)
revision = subprocess.check_output(
    ['git', 'rev-parse', '--verify', '--end-of-options', options.baseline + '^{commit}'],
    cwd=root, text=True).strip()
baseline = out / 'baseline-decoder.cpp'
baseline.write_bytes(subprocess.check_output(
    ['git', 'show', revision + ':src/audio/decoder.cpp'], cwd=root))
target = 'skin_sound_bundle_decode_tests'
commands = [entry for entry in json.loads((build / 'compile_commands.json').read_text())
            if f'CMakeFiles/{target}.dir/' in entry['command']]
if not commands:
    raise RuntimeError('Build skin_sound_bundle_decode_tests first')
objects = {}
jobs = []
baseline_job = None
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
    args = [arg for arg in args if arg not in
            ('-g', '-O0', '-O1', '-O2', '-O3', '-Os', '-Oz', '-Ofast')]
    args.extend(['-O2', '-g0', '-DASOBMASHOW_AUDIO_DECODE_BENCHMARK=1'])
    jobs.append((args, entry['directory']))
    if Path(entry['file']) == root / 'src/audio/decoder.cpp':
        previous = args.copy()
        previous[previous.index('-o') + 1] = str(out / 'baseline-decoder.o')
        previous[previous.index('-c') + 1] = str(baseline)
        previous.extend(['-iquote', str(root / 'src/audio')])
        for name in ('decodeAudioToPCM', 'decodeAudioToPCMBounded',
                     'decodeAudioBytesToPCM', 'decodeAudioBytesToPCMBounded',
                     'decodeSkinSoundBundleAware'):
            previous.append(f'-D{name}=benchmarkBaseline{name[0].upper()}{name[1:]}')
        baseline_job = (previous, entry['directory'])
if baseline_job is None:
    raise RuntimeError('Decoder compile command missing')
jobs.append(baseline_job)


def compile_source(job):
    args, cwd = job
    result = subprocess.run(args, cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(result.stdout)
    print('Compiled', args[args.index('-c') + 1], flush=True)


with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    list(pool.map(compile_source, jobs))
cache = (build / 'CMakeCache.txt').read_text().splitlines()
ninja = next(line.split('=', 1)[1] for line in cache
             if line.startswith('CMAKE_MAKE_PROGRAM:FILEPATH='))
link = subprocess.check_output([ninja, '-C', str(build), '-t', 'commands', target],
                               text=True).splitlines()[-1]
tokens = shlex.split(link)
if tokens[:2] != [':', '&&'] or tokens[-2:] != ['&&', ':'] or tokens.count('&&') != 2:
    raise RuntimeError('Unsupported link command; expected the CMake Unix Ninja layout')
args = [objects.get(arg, arg) for arg in tokens[2:-2]]
args.insert(1, str(out / 'baseline-decoder.o'))
args[args.index('-o') + 1] = str(out / 'benchmark')
subprocess.run(args, cwd=build, check=True)
manifest = {
    'baseline': revision, 'audio_paths': files,
    'baseline_decoder_sha256': hashlib.sha256(baseline.read_bytes()).hexdigest(),
    'current_decoder_sha256': hashlib.sha256((root / 'src/audio/decoder.cpp').read_bytes()).hexdigest(),
    'compile_commands': [args for args, _ in jobs], 'link_command': args,
    'dependency_build': str(build),
}
(out / 'commands.json').write_text(json.dumps(manifest, indent=2) + '\n')
log = out / 'benchmark.log'
with log.open('w') as stream:
    subprocess.run([str(out / 'benchmark'), '--benchmark', *files], cwd=root,
                   stdout=stream, stderr=subprocess.STDOUT, check=True)
print('Benchmark results:', log, flush=True)
