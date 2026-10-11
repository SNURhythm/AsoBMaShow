#!/usr/bin/env python3
"""Compare baseline/current concurrent ZIP readers using private local assets.

Requires an existing Unix Ninja build of archive_file_concurrency_tests. Creates
DEFLATE ZIP fixtures and optimized executables outside the repository. All private
paths, source snapshots, commands and raw results stay in that output directory.
Counts ordinary C++ allocations (not malloc or resident memory) during each read.
"""

import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shlex
import statistics
import subprocess
import tempfile
import zipfile

HARNESS = r'''
#include "ArchiveFile.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>

namespace probe {
std::atomic_bool enabled{false};
std::atomic_size_t count{0}, bytes{0};
}
void *operator new(std::size_t size) {
  if (probe::enabled.load(std::memory_order_relaxed)) {
    probe::count.fetch_add(1, std::memory_order_relaxed);
    probe::bytes.fetch_add(size, std::memory_order_relaxed);
  }
  if (void *p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

int main(int argc, char **argv) {
  if (argc < 5) return 2;
  const std::filesystem::path zip = argv[1];
  const auto workers = std::stoul(argv[2]);
  const auto rounds = std::stoul(argv[3]);
  std::vector<std::string> expected;
  std::vector<std::filesystem::path> paths;
  for (int i = 4; i < argc; ++i) {
    std::ifstream input(argv[i], std::ios::binary);
    if (!input) return 3;
    expected.emplace_back(std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>());
    paths.emplace_back(std::to_string(i - 4) + ".asset");
  }
  std::string error;
  std::vector<archive_file::Entry> entries;
  if (!archive_file::listEntries(zip, entries, &error)) return 4;
  std::atomic_size_t delivered{0};
  auto read = [&] {
    return archive_file::readArchiveEntriesConcurrently(zip, paths,
        [&](archive_file::FileData &&file) {
          const auto index = std::stoul(file.path.stem().string());
          const auto &source = expected.at(index);
          if (file.bytes.size() != source.size() ||
              (!source.empty() && std::memcmp(file.bytes.data(), source.data(), source.size())))
            throw std::runtime_error("ZIP bytes differ from original asset");
          ++delivered;
          return true;
        }, workers, 1024ull * 1024 * 1024, &error);
  };
  if (!read()) return 5;
  delivered = 0;
  probe::enabled = true;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < rounds; ++i) {
    if (!read()) { std::cerr << error; return 6; }
  }
  const auto end = std::chrono::steady_clock::now();
  probe::enabled = false;
  if (delivered != rounds * paths.size()) return 7;
  std::cout << "ZIP_BENCH "
            << std::chrono::duration<double, std::milli>(end - start).count() / rounds
            << ' ' << probe::count.load() / rounds
            << ' ' << probe::bytes.load() / rounds << ' ' << delivered << '\n';
}
'''

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', required=True)
parser.add_argument('--files-from', type=Path, required=True)
parser.add_argument('--build-dir', type=Path, default=root / 'cmake-build-debug')
parser.add_argument('--output', type=Path)
parser.add_argument('--rounds', type=int, default=8)
parser.add_argument('--samples', type=int, default=7)
parser.add_argument('--batch-only', action='store_true', help='Skip individual-asset cases')
options = parser.parse_args()
if options.rounds < 1 or options.samples < 1:
    raise ValueError('Round/sample counts must be positive')
build = options.build_dir.resolve()
out = (options.output or Path(tempfile.mkdtemp(prefix='asobmashow-zip-'))).resolve()
if out.is_relative_to(build) or out.is_relative_to(root):
    raise ValueError('--output must be outside the repository and dependency build')
files = [str(Path(line).expanduser().resolve())
         for line in options.files_from.read_text().splitlines() if line]
if not files or any(not Path(path).is_file() for path in files):
    raise ValueError('--files-from must contain existing asset files')
out.mkdir(parents=True, exist_ok=True)
revision = subprocess.check_output(
    ['git', 'rev-parse', '--verify', '--end-of-options', options.baseline + '^{commit}'],
    cwd=root, text=True).strip()
baseline = out / 'baseline-ArchiveFile.cpp'
baseline.write_bytes(subprocess.check_output(
    ['git', 'show', revision + ':src/ArchiveFile.cpp'], cwd=root))
harness = out / 'benchmark.cpp'
harness.write_text(HARNESS)
target = 'archive_file_concurrency_tests'
commands = [entry for entry in json.loads((build / 'compile_commands.json').read_text())
            if f'CMakeFiles/{target}.dir/' in entry['command']]
objects, jobs = {}, []
archive_object = None
for entry in commands:
    args = shlex.split(entry['command'])
    original = args[args.index('-o') + 1]
    destination = (out / original).resolve()
    if not destination.is_relative_to(out):
        raise ValueError('Object path escapes output directory')
    destination.parent.mkdir(parents=True, exist_ok=True)
    objects[original] = str(destination)
    args[args.index('-o') + 1] = str(destination)
    args = [arg for arg in args if arg not in
            ('-g', '-O0', '-O1', '-O2', '-O3', '-Os', '-Oz', '-Ofast')]
    args.extend(['-O2', '-g0'])
    if Path(entry['file']) == root / 'tests/archive_file_concurrency_tests.cpp':
        args[args.index('-c') + 1] = str(harness)
    jobs.append((args, entry['directory']))
    if Path(entry['file']) == root / 'src/ArchiveFile.cpp':
        archive_object = str(destination)
        previous = args.copy()
        previous[previous.index('-o') + 1] = str(out / 'baseline-ArchiveFile.o')
        previous[previous.index('-c') + 1] = str(baseline)
        previous.extend(['-iquote', str(root / 'src')])
        jobs.append((previous, entry['directory']))
if archive_object is None:
    raise RuntimeError('Build archive_file_concurrency_tests first')


def compile_source(job):
    args, cwd = job
    result = subprocess.run(args, cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(result.stdout)


with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    list(pool.map(compile_source, jobs))
cache = (build / 'CMakeCache.txt').read_text().splitlines()
ninja = next(line.split('=', 1)[1] for line in cache
             if line.startswith('CMAKE_MAKE_PROGRAM:FILEPATH='))
link = subprocess.check_output([ninja, '-C', str(build), '-t', 'commands', target],
                               text=True).splitlines()[-1]
tokens = shlex.split(link)
if tokens[:2] != [':', '&&'] or tokens[-2:] != ['&&', ':'] or tokens.count('&&') != 2:
    raise RuntimeError('Unsupported link command; expected CMake Unix Ninja layout')
links = []
for variant in ('baseline', 'current'):
    args = [objects.get(arg, arg) for arg in tokens[2:-2]]
    if variant == 'baseline':
        args[args.index(archive_object)] = str(out / 'baseline-ArchiveFile.o')
    args[args.index('-o') + 1] = str(out / variant)
    subprocess.run(args, cwd=build, check=True, stdout=subprocess.DEVNULL)
    links.append(args)
(out / 'commands.json').write_text(json.dumps({
    'baseline': revision, 'files': files, 'dependency_build': str(build),
    'rounds': options.rounds, 'samples': options.samples, 'batch_only': options.batch_only,
    'baseline_archive_sha256': hashlib.sha256(baseline.read_bytes()).hexdigest(),
    'current_archive_sha256': hashlib.sha256((root / 'src/ArchiveFile.cpp').read_bytes()).hexdigest(),
    'compile_commands': [args for args, _ in jobs], 'link_commands': links,
}, indent=2) + '\n')
results = []
# Individual entries expose size-dependent costs; the batch exercises parallel loading.
cases = [] if options.batch_only else [(f'asset-{i}', [path]) for i, path in enumerate(files)]
cases.append(('batch', files))
for case, assets in cases:
    archive = out / (case + '.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for i, path in enumerate(assets):
            z.write(path, f'{i}.asset')
    with zipfile.ZipFile(archive) as z:
        compressed = [entry.compress_size for entry in z.infolist()]
        uncompressed = sum(entry.file_size for entry in z.infolist())
    # Public concurrent API requires at least two requested workers. Single-entry
    # cases internally clamp that request to one active extraction worker.
    for workers in ([2, 4] if case == 'batch' else [2]):
        records = {'baseline': [], 'current': []}
        for sample in range(options.samples):
            for variant in (('baseline', 'current') if sample % 2 == 0 else ('current', 'baseline')):
                output = subprocess.check_output(
                    [str(out / variant), str(archive), str(workers), str(options.rounds), *assets],
                    cwd=root, text=True, stderr=subprocess.STDOUT)
                with (out / 'raw.log').open('a') as log:
                    log.write(f'{case} {workers} {variant} {sample}\n{output}\n')
                line = next(line for line in output.splitlines() if line.startswith('ZIP_BENCH '))
                ms, count, byte_count, checked = map(float, line.split()[1:])
                records[variant].append(dict(ms=ms, allocations=count, bytes=byte_count, checked=checked))
        row = dict(case=case, workers=workers, output_bytes=uncompressed,
                   compressed_bytes=sum(compressed),
                   samples=records)
        for variant, samples in records.items():
            row[variant] = {key: statistics.median(s[key] for s in samples)
                            for key in ('ms', 'allocations', 'bytes')}
        results.append(row)
        print(json.dumps({key: value for key, value in row.items() if key != 'samples'}), flush=True)
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
print('Results:', out / 'results.json', flush=True)
