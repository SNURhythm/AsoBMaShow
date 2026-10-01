#!/usr/bin/env python3
"""Opt-in macOS/Ninja text-library experiment; leaves production sources untouched.

Requires a configured/built skin_draw_command_tests target and network access on
first run. Dependencies are release-pinned and hash-checked. Outputs (including
source snapshots) live outside the checkout by default.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import shlex
import shutil
import statistics
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "utf8proc": ("JuliaStrings/utf8proc", "v2.11.3", "abfed50b6d4da51345713661370290f4f4747263ee73dc90356299dfc7990c78"),
    "simdutf": ("simdutf/simdutf", "v9.2.1", "582f9d0dcf578f6d4766fa29ea12a7f2f02bd3c6ad9e0cf35a8e0ec8478eba4b"),
    "utfcpp": ("nemtrif/utfcpp", "v4.2.1", "6d6a5493a111884cc085ee31babfe6d9960c8fb08fc80a64852eaeea8323dbc1"),
}
BACKENDS = {"utf8proc": 1, "simdutf": 2, "utfcpp": 3, "production": 4}
COMMAND_LOG = []
SIMD_FLAGS = [f"-DSIMDUTF_FEATURE_{feature}=0" for feature in
              ("UTF16", "ASCII", "LATIN1", "BASE64", "DETECT_ENCODING")]


def run(args, cwd=ROOT):
    COMMAND_LOG.append(dict(argv=list(map(str, args)), cwd=str(cwd)))
    result = subprocess.run([str(a) for a in args], cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"{shlex.join(map(str, args))}\n{result.stdout}")
    return result.stdout


def download(name, output):
    repo, tag, digest = SOURCES[name]
    archive = output / f"{name}-{tag}.tar.gz"
    if not archive.exists():
        url = f"https://codeload.github.com/{repo}/tar.gz/refs/tags/{tag}"
        with urllib.request.urlopen(url, timeout=60) as response:
            archive.write_bytes(response.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise RuntimeError(f"Archive checksum mismatch: {archive}")
    parent = output / "sources" / name
    parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as source:
        source.extractall(parent, filter="data")
    return parent / f"{repo.split('/')[-1]}-{tag.removeprefix('v')}"


def build_experiment(options, output, build):
    deps = {name: download(name, output) for name in SOURCES}
    cxx = os.environ.get("CXX", "c++")
    cc = os.environ.get("CC", "cc")
    includes = [f"-I{deps['utf8proc']}", f"-I{deps['simdutf'] / 'include'}",
                f"-I{deps['utfcpp'] / 'source'}", f"-I{output / 'snapshot/src'}"]
    utf8_object = output / "utf8proc.o"
    simd_object = output / "simdutf.o"
    run([cc, "-O3", "-g0", "-DNDEBUG", "-DUTF8PROC_STATIC", "-c",
         deps["utf8proc"] / "utf8proc.c", "-o", utf8_object])
    run([cxx, "-std=c++20", "-O3", "-g0", "-DNDEBUG", *SIMD_FLAGS, *includes,
         f"-I{deps['simdutf'] / 'src'}", "-c",
         deps["simdutf"] / "src/simdutf.cpp", "-o", simd_object])

    # A private source snapshot supplies alternate cache-miss decoders. Every
    # translation unit in each executable sees the same cache definition.
    snapshot = output / "snapshot"
    shutil.copytree(ROOT / "src", snapshot / "src", dirs_exist_ok=True)
    adapter = ROOT / "tests/benchmarks/TextLibraryAdapters.h"
    shutil.copy2(adapter, snapshot / "src/skin/beatoraja/TextLibraryAdapters.h")
    cache = snapshot / "src/skin/beatoraja/SkinTextDecodeCache.h"
    text = cache.read_text()
    start = text.index("    if (!asobmashow::text::decodeUtf8(value, *decoded)) return {};")
    end = text.index("    entry.text.assign(value);", start)
    text = text[:start] + "    if (!text_library_experiment::decode(value, *decoded)) return {};\n\n" + text[end:]
    cache.write_text(text.replace('#include "../../text/Utf8.h"', '#include "TextLibraryAdapters.h"'))
    runtime_object = output / "Utf8.o"
    run([cxx, "-std=c++20", "-O3", "-g0", "-DNDEBUG", "-c",
         snapshot / "src/text/Utf8.cpp", "-o", runtime_object])

    database = json.loads((build / "compile_commands.json").read_text())
    commands = {}
    for item in database:
        args = shlex.split(item["command"])
        if "-o" in args:
            commands[args[args.index("-o") + 1]] = (args, item["directory"])
    raw = run(["ninja", "-C", build, "-t", "commands", "skin_draw_command_tests"])
    link_lines = [line for line in raw.splitlines() if " -o skin_draw_command_tests " in line]
    if len(link_lines) != 1:
        raise RuntimeError("Expected a single Ninja skin_draw_command_tests link command")
    tokens = shlex.split(link_lines[0])
    # CMake/Ninja wraps the linker with ': && ... && :'. Never execute a shell.
    if tokens[:2] != [":", "&&"] or tokens[-2:] != ["&&", ":"]:
        raise RuntimeError("Unrecognized Ninja linker wrapper")
    link = tokens[2:-2]
    objects = [item for item in link if item.endswith(".o")]

    def compile_one(task):
        backend, obj = task
        args, cwd = commands[obj]
        args = [arg.replace(str(ROOT / "src"), str(snapshot / "src")) for arg in args]
        target = output / backend / obj.replace("/", "_")
        args[args.index("-o") + 1] = str(target)
        args += ["-O3", "-g0", "-DNDEBUG", f"-DTEXT_LIBRARY_BACKEND={BACKENDS[backend]}", *SIMD_FLAGS]
        # Put pinned dependency headers before existing vcpkg includes.
        args[1:1] = includes
        run(args, cwd)
        return backend, obj, str(target)

    for backend in BACKENDS:
        (output / backend).mkdir(exist_ok=True)
    print("Compiling optimized renderer variants", flush=True)
    tasks = [(backend, obj) for backend in BACKENDS for obj in objects]
    replacements = {backend: {} for backend in BACKENDS}
    with concurrent.futures.ThreadPoolExecutor(max_workers=options.jobs) as pool:
        for backend, obj, target in pool.map(compile_one, tasks):
            replacements[backend][obj] = target
    for backend, number in BACKENDS.items():
        extra_objects = ([simd_object] if backend == "simdutf" else
                         [runtime_object] if backend == "production" else [])
        args = [replacements[backend].get(arg, arg) for arg in link]
        utf8_links = [i for i, arg in enumerate(args) if arg.endswith("/libutf8proc.a")]
        if len(utf8_links) != 1:
            raise RuntimeError("Expected one utf8proc static library in renderer link")
        args[utf8_links[0]] = str(utf8_object)
        args[args.index("-o") + 1] = str(output / backend / "renderer")
        runtime_links = [i for i, arg in enumerate(args)
                         if arg.endswith("libasobmashow_utf8.a")]
        if len(runtime_links) != 1:
            raise RuntimeError("Expected one shared UTF-8 library in renderer link")
        args[runtime_links[0]] = str(runtime_object)
        # The runtime object supplies simdutf symbols for every renderer variant.
        run(args, build)
        flags = ["-std=c++20", "-O3", "-g0", "-DNDEBUG", "-DUTF8PROC_STATIC",
                 f"-DTEXT_LIBRARY_BACKEND={number}", *SIMD_FLAGS, *includes]
        run([cxx, *flags, ROOT / "tests/benchmarks/text_library_benchmark.cpp",
             utf8_object, *extra_objects, "-o", output / backend / "operations"])
        run([cxx, f"-I{snapshot / 'src'}", *flags,
             ROOT / "tests/skin_text_decode_cache_tests.cpp", utf8_object,
             *extra_objects, "-o", output / backend / "cache_tests"])
    compiler_names = {cc, cxx, *(commands[obj][0][0] for obj in objects)}
    compiler_identities = {name: run([name, "--version"]).strip()
                           for name in sorted(compiler_names)}
    manifest = dict(
        revision=run(["git", "rev-parse", "HEAD"]).strip(),
        compilers=compiler_identities, commands=list(COMMAND_LOG), platform=platform.platform(),
        versions=SOURCES, flags=["-O3", "-g0", "-DNDEBUG", *SIMD_FLAGS],
        source_sha256={str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in (ROOT / "src/skin/beatoraja/SkinTextDecodeCache.h",
                                    ROOT / "src/skin/beatoraja/Skin2DRenderer.cpp",
                                    ROOT / "tests/skin_draw_command_tests.cpp",
                                    ROOT / "src/text/Utf8.cpp", ROOT / "src/text/Utf8.h",
                                    ROOT / "src/text/simdutf/simdutf.h",
                                    ROOT / "src/text/simdutf/simdutf.cpp.inc", adapter,
                                    ROOT / "tests/benchmarks/text_library_benchmark.cpp",
                                    Path(__file__).resolve())})
    (output / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def measure(options, output):
    checks = {}
    for backend in BACKENDS:
        checks[backend] = {}
        for name in ("operations", "cache_tests", "renderer"):
            checks[backend][name] = run([output / backend / name])
        print(checks[backend]["operations"].strip(), flush=True)
    rows = []
    rng = random.Random(20261001)
    for sample in range(options.samples):
        order = list(BACKENDS)
        rng.shuffle(order)
        for backend in order:
            log = run([output / backend / "operations", "--benchmark"])
            for line in log.splitlines():
                match = re.fullmatch(r"operation backend=(\w+) case=(\w+) bytes=(\d+) kind=(\w+) ns=([\d.]+) checksum=(\d+)", line)
                if not match:
                    raise RuntimeError(f"Unexpected operation output: {line}")
                name, case, size, kind, nanos, checksum = match.groups()
                if name != backend:
                    raise RuntimeError("Operation binary/backend mismatch")
                rows.append(dict(sample=sample, backend=name, case=case, kind=kind,
                                 ns=float(nanos), bytes=int(size), checksum=int(checksum)))
            log = run([output / backend / "renderer", "--benchmark-text"])
            frames = re.findall(r"text_frames (\w+) us/frame=([\d.]+) glyphs=(\d+)", log)
            if sorted(case for case, _, _ in frames) != ["changing", "stable"]:
                raise RuntimeError("Expected stable and changing renderer measurements")
            for case, micros, glyphs in frames:
                rows.append(dict(sample=sample, backend=backend, case=case,
                                 kind="frame", ns=float(micros) * 1000, checksum=int(glyphs)))
        print(f"Measured sample {sample + 1}/{options.samples}", flush=True)
    summary = []
    for kind, case in sorted({(row["kind"], row["case"]) for row in rows}):
        group = [row for row in rows if row["kind"] == kind and row["case"] == case]
        if len({row["checksum"] for row in group}) != 1:
            raise RuntimeError(f"Output mismatch in {kind}/{case}")
        medians = {}
        ranges = {}
        for backend in BACKENDS:
            values = [row["ns"] for row in group if row["backend"] == backend]
            if len(values) != options.samples:
                raise RuntimeError(f"Missing samples for {backend}/{kind}/{case}")
            medians[backend] = statistics.median(values)
            ranges[backend] = [min(values), max(values)]
        summary.append(dict(kind=kind, case=case, median_ns=medians, range_ns=ranges))
        print(kind, case, medians, flush=True)
    result = dict(build=json.loads((output / "build-manifest.json").read_text()),
                  measurement_platform=platform.platform(),
                  samples=options.samples, correctness=checks,
                  binary_bytes={b: (output / b / "renderer").stat().st_size for b in BACKENDS},
                  rows=rows, summary=summary)
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "cmake-build-debug")
    parser.add_argument("--output", type=Path, default=Path("/tmp/asobmashow-text-libraries"))
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--skip-build", action="store_true", help="remeasure existing experiment binaries")
    options = parser.parse_args()
    if options.samples < 1 or options.jobs < 1:
        parser.error("samples and jobs must be positive")
    output, build = options.output.resolve(), options.build_dir.resolve()
    if output == ROOT or ROOT in output.parents:
        parser.error("experiment output must be outside the checkout")
    output.mkdir(parents=True, exist_ok=True)
    if not options.skip_build:
        print("Building pinned text libraries", flush=True)
        build_experiment(options, output, build)
    measure(options, output)
    print(f"Results: {output / 'results.json'}")


if __name__ == "__main__":
    main()
