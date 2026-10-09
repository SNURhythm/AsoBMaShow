#!/usr/bin/env python3
"""Build the isolated native-row comparison from a configured macOS Ninja tree."""
import argparse
import concurrent.futures
import hashlib
import io
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--baseline', default='1517a9237')
parser.add_argument('--candidate', default='HEAD')
options = parser.parse_args()
root, build, output = (path.resolve() for path in
                       (options.root, options.build_dir, options.output))
if output == root or root in output.parents or output.exists():
    parser.error('--output must be a new directory outside the checkout')
changed = subprocess.check_output(
    ['git', 'diff', '--name-only', options.baseline, options.candidate, '--', 'src'],
    cwd=root, text=True).splitlines()
expected = {'src/view/ImageFileDecoder.cpp', 'src/view/ImageRowReducer.h',
            'src/view/PngRowDecoder.h'}
if set(changed) != expected:
    parser.error('comparison expects exactly the three native-row production changes')
output.mkdir(parents=True)
manifest = []

def run(args, cwd):
    manifest.append({'argv': list(map(str, args)), 'cwd': str(cwd)})
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout

for name, revision in (('before', options.baseline), ('after', options.candidate)):
    snapshot = output / name
    snapshot.mkdir()
    archive = subprocess.check_output(['git', 'archive', revision, 'src'], cwd=root)
    with tarfile.open(fileobj=io.BytesIO(archive)) as source:
        source.extractall(snapshot, filter='data')
    for directory in ('bgfx', 'SDL', 'SDL_ttf', 'third_party', 'include'):
        (snapshot / directory).symlink_to(root / directory, target_is_directory=True)

# Freeze one benchmark TU for both variants; its support header and fixture
# assets come from the same checkout. No skin files are copied into evidence.
probe = output / 'probe.cpp'
shutil.copy2(root / 'tests/gameplay_skin_loading_benchmark_tests.cpp', probe)
commands = {}
for item in json.loads((build / 'compile_commands.json').read_text()):
    args = shlex.split(item['command'])
    if '-o' in args:
        commands[args[args.index('-o') + 1]] = (args, item['directory'])
raw = run(['ninja', '-C', str(build), '-t', 'commands',
           'gameplay_skin_loading_benchmark_tests'], root)
lines = [line for line in raw.splitlines()
         if ' -o gameplay_skin_loading_benchmark_tests ' in line]
if len(lines) != 1:
    raise RuntimeError('expected one benchmark link command')
tokens = shlex.split(lines[0])
if tokens[:2] != [':', '&&'] or tokens[-2:] != ['&&', ':']:
    raise RuntimeError('unexpected Ninja link wrapper')
link = tokens[2:-2]
objects = [arg for arg in link if arg.endswith('.o')]

def compile_one(obj):
    args, cwd = commands[obj]
    args = [arg.replace(str(root / 'src'), str(output / 'before/src'))
            for arg in args]
    if args[args.index('-c') + 1].endswith('/gameplay_skin_loading_benchmark_tests.cpp'):
        args[args.index('-c') + 1] = str(probe)
        args += ['-iquote', str(root / 'tests')]
    destination = output / ('before-' + obj.replace('/', '_'))
    args[args.index('-o') + 1] = str(destination)
    args += ['-O3', '-gline-tables-only', '-DNDEBUG']
    run(args, cwd)
    return obj, str(destination)

with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    replacements = dict(pool.map(compile_one, objects))
baseline_link = [replacements.get(arg, arg) for arg in link]
baseline_link[baseline_link.index('-o') + 1] = str(output / 'probe-before')
run(baseline_link, build)

# The changed row helpers are used only by this TU in the benchmark target.
# Keep every other object and dependency bit-identical between the variants.
image_obj = next(obj for obj in objects if obj.endswith('/ImageFileDecoder.cpp.o'))
args, cwd = commands[image_obj]
args = [arg.replace(str(root / 'src'), str(output / 'after/src')) for arg in args]
candidate_obj = output / 'candidate-image.o'
args[args.index('-o') + 1] = str(candidate_obj)
args += ['-O3', '-gline-tables-only', '-DNDEBUG']
run(args, cwd)
candidate_link = baseline_link.copy()
candidate_link[candidate_link.index(replacements[image_obj])] = str(candidate_obj)
candidate_link[candidate_link.index('-o') + 1] = str(output / 'probe-after')
run(candidate_link, build)
(output / 'build.json').write_text(json.dumps({
    'commands': manifest,
    'probeSha256': hashlib.sha256(probe.read_bytes()).hexdigest(),
    'baseline': subprocess.check_output(['git', 'rev-parse', options.baseline], cwd=root, text=True).strip(),
    'candidate': subprocess.check_output(['git', 'rev-parse', options.candidate], cwd=root, text=True).strip(),
}, indent=2) + '\n')
