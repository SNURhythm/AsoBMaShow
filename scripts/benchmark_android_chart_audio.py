#!/usr/bin/env python3
"""Run headless chart audio preparation against a built Android production library.

Build first with scripts/android_firebase_deploy.sh --build-only. Supply the
matching arm64 CMake build directory, a directory containing its runtime .so
files, and a private text file of absolute chart paths already on the device.
This does not install an APK, access SAF, or measure the UI/audio backend.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import uuid

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--device', required=True, help='Explicit adb serial')
parser.add_argument('--adb', default='adb')
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--libraries', type=Path, required=True, help='Matching arm64 runtime library directory')
parser.add_argument('--charts-from', type=Path, required=True, help='One absolute device chart path per line')
parser.add_argument('--output', type=Path)
parser.add_argument('--rounds', type=int, default=3)
parser.add_argument('--sample-rates', type=int, nargs='+', default=[48000], choices=[44100, 48000])
parser.add_argument('--cache-bytes', type=int, help='Probe a retention budget; default is unlimited')
options = parser.parse_args()
if not 1 <= options.rounds <= 100 or (options.cache_bytes is not None and options.cache_bytes < 0):
    parser.error('rounds must be 1..100 and cache-bytes must be nonnegative')
charts = options.charts_from.read_text().splitlines()
if not charts or any(not path.startswith('/') for path in charts):
    parser.error('--charts-from must contain absolute device paths')
build, libraries = options.build_dir.resolve(), options.libraries.resolve()
out = (options.output or Path(tempfile.mkdtemp(prefix='asobmashow-android-audio-'))).resolve()
if out.is_relative_to(root) or out == build or out == libraries:
    parser.error('--output must be outside the checkout/build/library directories')
out.mkdir(parents=True, exist_ok=True)
entries = json.loads((build / 'compile_commands.json').read_text())
entry = next(item for item in entries if item['file'].endswith('/src/audio/Jukebox.cpp'))
args = shlex.split(entry['command'])
source = root / 'tests/chart_audio_preparation_benchmark.cpp'
args[args.index('-c') + 1] = str(source)
args[args.index('-o') + 1] = str(out / 'benchmark.o')
args.extend(['-I' + str(root / 'src'), '-I' + str(root / 'bgfx/bgfx/include'),
             '-I' + str(root / 'bgfx/bx/include'), '-I' + str(root / 'bgfx/bimg/include'),
             '-I' + str(root / 'bgfx/bgfx/3rdparty')])
subprocess.run(args, cwd=entry['directory'], check=True)
link = [args[0], *[arg for arg in args if arg.startswith(('--target=', '--sysroot='))],
        str(out / 'benchmark.o'), '-L' + str(libraries), '-lmain', '-lSDL3',
        '-Wl,-rpath-link,' + str(libraries), '-Wl,-rpath,$ORIGIN', '-Wl,--export-dynamic',
        '-o', str(out / 'benchmark')]
subprocess.run(link, check=True)
# Keep local snapshots for comparison. Remove debug sections only from these
# copies, preserving exported symbols needed by the headless executable.
runtime = out / 'runtime'
runtime.mkdir(exist_ok=True)
strip = str(Path(args[0]).with_name('llvm-strip'))
for name in ('libmain.so', 'libSDL3.so', 'libSDL3_ttf.so', 'libc++_shared.so'):
    shutil.copy2(libraries / name, runtime / name)
    subprocess.run([strip, '--strip-debug', str(runtime / name)], check=True)
adb = [options.adb, '-s', options.device]
remote = '/data/local/tmp/asobmashow-chart-audio-' + uuid.uuid4().hex
manifest = {
    'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
    'device': options.device, 'charts': charts, 'rounds': options.rounds,
    'sample_rates': options.sample_rates, 'cache_bytes': options.cache_bytes,
    'compile_command': args, 'link_command': link,
    'benchmark_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'runtime_sha256': {file.name: hashlib.sha256(file.read_bytes()).hexdigest()
                       for file in runtime.iterdir()},
    'fingerprint': subprocess.check_output(adb + ['shell', 'getprop', 'ro.build.fingerprint'], text=True).strip(),
}
(out / 'commands.json').write_text(json.dumps(manifest, indent=2) + '\n')
subprocess.run(adb + ['shell', 'mkdir', remote], check=True)
try:
    for file in [out / 'benchmark', *runtime.iterdir()]:
        subprocess.run(adb + ['push', str(file), remote + '/'], check=True)
    for rate in options.sample_rates:
        environment = ['LD_LIBRARY_PATH=.']
        if options.cache_bytes is not None:
            environment.append('ASOBMASHOW_BENCHMARK_CACHE_BYTES=' + str(options.cache_bytes))
        command = ('cd ' + shlex.quote(remote) + ' && ' + ' '.join(environment) + ' ' +
                   shlex.join(['./benchmark', '--benchmark-chart-audio', str(rate), str(options.rounds), *charts]))
        log = out / f'benchmark-{rate}.log'
        with log.open('w') as stream:
            subprocess.run(adb + ['shell', command], stdout=stream, stderr=subprocess.STDOUT, check=True)
        print('Benchmark results:', log, flush=True)
finally:
    # Only this invocation's newly created binary directory; never chart inputs.
    subprocess.run(adb + ['shell', 'rm', '-rf', remote], check=False)
