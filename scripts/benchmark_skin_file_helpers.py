#!/usr/bin/env python3
"""Compare the former scalar validator with production in real Lua file helpers.

First run benchmark_text_libraries.py with the same --output directory. Renderer
objects from that immutable source snapshot are reused for shared dependencies.
"""

import argparse
import concurrent.futures
import json
import hashlib
from pathlib import Path
import random
import re
import shlex

from benchmark_text_libraries import COMMAND_LOG, ROOT, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('/tmp/asobmashow-text-adoption'))
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'cmake-build-debug')
    parser.add_argument('--samples', type=int, default=7)
    parser.add_argument('--skip-build', action='store_true')
    options = parser.parse_args()
    output, build = options.output.resolve(), options.build_dir.resolve()
    if output == ROOT or ROOT in output.parents or options.samples < 1:
        parser.error('output must be external and samples positive')
    snapshot = output / 'snapshot/src'
    manifest = json.loads((output / 'build-manifest.json').read_text())
    target = output / 'file-helpers'
    target.mkdir(exist_ok=True)
    if not options.skip_build:
        database = json.loads((build / 'compile_commands.json').read_text())
        commands = {}
        for item in database:
            args = shlex.split(item['command'])
            if '-o' in args:
                commands[args[args.index('-o') + 1]] = (args, item['directory'])
        raw = run(['ninja', '-C', build, '-t', 'commands', 'lua_skin_host_modules_tests'])
        lines = [line for line in raw.splitlines() if ' -o lua_skin_host_modules_tests ' in line]
        if len(lines) != 1:
            raise RuntimeError('Expected one host test link command')
        tokens = shlex.split(lines[0])
        if tokens[:2] != [':', '&&'] or tokens[-2:] != ['&&', ':']:
            raise RuntimeError('Unexpected Ninja linker wrapper')
        link = tokens[2:-2]
        objects = [arg for arg in link if arg.endswith('.o')]
        # Reuse the already optimized renderer's exact commands when possible.
        recorded = {entry['argv'][entry['argv'].index('-o') + 1]: entry
                    for entry in manifest['commands'] if '-c' in entry['argv'] and '-o' in entry['argv']}
        def compile_one(obj):
            args, cwd = commands[obj]
            candidate = output / 'production' / obj.replace('/', '_')
            entry = recorded.get(str(candidate))
            # The host test TU changed to add its optional benchmark, so compile
            # it from the live checkout; all production sources use the snapshot.
            if entry and candidate.exists():
                return obj, str(candidate)
            args = [arg.replace(str(ROOT / 'src'), str(snapshot)) for arg in args]
            dest = target / obj.replace('/', '_')
            args[args.index('-o') + 1] = str(dest)
            args += ['-O3', '-g0', '-DNDEBUG', '-DTEXT_LIBRARY_BACKEND=4']
            run(args, cwd)
            return obj, str(dest)
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            replacements = dict(pool.map(compile_one, objects))
        args = [replacements.get(arg, arg) for arg in link]
        for i, arg in enumerate(args):
            if arg.endswith('/libutf8proc.a'): args[i] = str(output / 'utf8proc.o')
            if arg.endswith('libasobmashow_utf8.a'): args[i] = str(output / 'Utf8.o')
        args[args.index('-o') + 1] = str(target / 'production')
        run(args, build)
        # Restore just the pre-adoption scalar validator in a private host TU.
        old = run(['git', 'show', 'f380b5bf:src/skin/beatoraja/LuaSkinHostModules.cpp'])
        start = old.index('bool validUtf8FileContents(')
        validator = old[start:old.index('\n}\n', start) + 3]
        host_path = snapshot / 'skin/beatoraja/LuaSkinHostModules.cpp'
        baseline = target / 'LuaSkinHostModules.cpp'
        source = host_path.read_text().replace('asobmashow::text::validUtf8(', 'validUtf8FileContents(')
        source = source.replace('namespace {', 'namespace {\n' + validator, 1)
        baseline.write_text(source)
        host_obj = next(obj for obj in objects if obj.endswith('/LuaSkinHostModules.cpp.o'))
        command, cwd = commands[host_obj]
        command = [arg.replace(str(ROOT / 'src'), str(snapshot)) for arg in command]
        command[command.index('-c') + 1] = str(baseline)
        command[command.index('-o') + 1] = str(target / 'baseline-host.o')
        command += ['-O3', '-g0', '-DNDEBUG', '-DTEXT_LIBRARY_BACKEND=4', '-iquote', str(host_path.parent)]
        run(command, cwd)
        args[args.index(replacements[host_obj])] = str(target / 'baseline-host.o')
        args[args.index('-o') + 1] = str(target / 'scalar')
        run(args, build)
        sources = (Path(__file__).resolve(), ROOT / 'tests/lua_skin_host_modules_tests.cpp',
                   host_path, baseline)
        (target / 'build-manifest.json').write_text(json.dumps(dict(
            renderer_build=manifest, commands=COMMAND_LOG,
            source_sha256={str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in sources}), indent=2) + '\n')
    rows = []
    rng = random.Random(20261001)
    for sample in range(options.samples):
        order = ['scalar', 'production']
        rng.shuffle(order)
        for backend in order:
            log = run([target / backend, '--benchmark-files'])
            matches = re.findall(r'benchmark_(count|read) us/file=([\d.]+)', log)
            if sorted(case for case, _ in matches) != ['count', 'read']:
                raise RuntimeError(log)
            rows.extend(dict(sample=sample, backend=backend, case=case, us=float(micros))
                        for case, micros in matches)
        print(f'Measured sample {sample + 1}/{options.samples}', flush=True)
    (target / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(target / 'results.json')


if __name__ == '__main__':
    main()
