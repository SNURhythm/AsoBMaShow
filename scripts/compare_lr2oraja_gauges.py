#!/usr/bin/env python3
"""Compare actual native gauges with pinned lr2oraja Java gauge classes.

Requires a C++23 compiler and Java 17+. All build artifacts stay in a temporary
folder. Model/utility stubs only supply data; gauge logic and TOTAL validation
come directly from the checked reference source files.
"""
import argparse
import itertools
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

PIN = "5233be081abee2a7f824b78aed0d783847deeb2f"
REPO = Path(__file__).resolve().parents[1]
FIXTURES = REPO / "tests/fixtures/lr2oraja_gameplay"


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, **kwargs)


def cases():
    # Ordinary LR2, default and explicit course constraints, and the two named
    # LR2 profiles under the separately selected upstream Beatoraja ruleset.
    configurations = [(0, 0), (0, 1), (0, 2), (0, 3), (0, 4),
                      (0, 5), (0, 6), (1, 10), (1, 6)]
    notes = [1, 19, 20, 21, 29, 30, 34, 59, 60, 124, 125, 138,
             249, 250, 253, 399, 400, 499, 500, 503, 599, 600,
             999, 1000, 1001, 3000]
    totals = [0, -1, .001, .5, .999, 95.999, 96, 111.999, 112,
              127.999, 128, 143.999, 144, 159.999, 160, 175.999,
              176, 191.999, 192, 207.999, 208, 223.999, 224,
              239.9, 239.999, 240, 200.5, 300]
    for ruleset, profile in configurations:
        gauges = range(9) if profile in (0, 10) else [2, 3, 4]
        for gauge, n, total, judge in itertools.product(gauges, notes, totals, range(6)):
            yield (ruleset, profile, gauge, n, total, 1, 20 if judge < 3 else 100, judge, 1, 0, 1)
        for gauge, start, judge, rate in itertools.product(
                gauges, [0, 1, 2, 5, 9, 10, 19, 20, 24, 25, 29, 30, 31,
                         32, 49, 50, 79, 80, 99, 100, 120],
                range(6), [.3, .5, .7, 1, 1.3]):
            yield (ruleset, profile, gauge, 1000, 239.9, 1, start, judge, rate, 0, 8)
        for gauge, start, damage in itertools.product(
                gauges, [0, 1, 2, 31, 32, 100], [-.001, -1, -2, -99, -200, .1, 100]):
            yield (ruleset, profile, gauge, 101, 0, 0, start, -1, 1, damage, 8)
        for gauge, start in itertools.product(gauges, [2, 20, 31, 32, 80, 100]):
            yield (ruleset, profile, gauge, 503, 200.5, 1, start, 6, .7, -.001, 500)
    # Sweep every integer note count through all MODIFY_DAMAGE segments.
    for ruleset, profile in [(0, 0), (1, 10)]:
        for n, gauge, judge, rate in itertools.product(range(1, 1002), [3, 4], [3, 4, 5], [1, .3]):
            yield (ruleset, profile, gauge, n, 240, 1, 31, judge, rate, 0, 1)
    yield (0, 0, 2, 1000, 239.9, 1, 20, 0, 1, 0, 251)
    yield (1, 10, 2, 1000, 239.9, 1, 20, 0, 1, 0, 251)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, default=REPO.parent / "lr2oraja-endlessdream")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--allow-unpinned", action="store_true")
    args = parser.parse_args()
    revision = run(["git", "-C", str(args.reference), "rev-parse", "HEAD"], capture_output=True).stdout.strip()
    if revision != PIN and not args.allow_unpinned:
        parser.error(f"reference revision {revision} differs from pinned {PIN}")
    java_home = args.java_home
    if java_home is None and Path("/usr/libexec/java_home").exists():
        java_home = Path(run(["/usr/libexec/java_home", "-v", "17"], capture_output=True).stdout.strip())
    java = str(java_home / "bin/java") if java_home else "java"
    javac = str(java_home / "bin/javac") if java_home else "javac"
    with tempfile.TemporaryDirectory(prefix="asobmashow-gauge-parity-") as directory:
        root = Path(directory)
        source = root / "java-src"
        for name, content in json.loads((FIXTURES / "gauge_reference_support.json").read_text()).items():
            file = source / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(content)
        reference_source = args.reference / "core/src/bms/player/beatoraja/play"
        for name in ["BMSPlayerRule.java", "GaugeProperty.java", "GrooveGauge.java"]:
            file = reference_source / name
            relative = str(file.relative_to(args.reference))
            pinned = subprocess.run(["git", "-C", str(args.reference), "show", f"{revision}:{relative}"],
                                    check=True, capture_output=True).stdout
            if file.read_bytes() != pinned:
                parser.error(f"reference file has uncommitted changes: {relative}")
            shutil.copy(file, source / "bms/player/beatoraja/play" / name)
        shutil.copy(FIXTURES / "gauge_reference_probe.java", source / "GaugeReferenceProbe.java")
        run([javac, "-d", str(root / "java-classes"), "-sourcepath", str(source),
             str(source / "GaugeReferenceProbe.java")])
        run([args.cxx, "-std=c++23", "-O2", "-I", str(REPO / "src"),
             str(FIXTURES / "gauge_reference_probe.cpp"),
             str(REPO / "src/scene/play/GameplayGaugeRules.cpp"),
             "-o", str(root / "gauge-native")])
        count = 0
        mismatches = 0
        with (root / "input").open("w") as output:
            for case in cases():
                output.write(" ".join(map(str, case)) + "\n")
                count += 1
        for name, command in [("native", [str(root / "gauge-native")]),
                              ("reference", [java, "-cp", str(root / "java-classes"), "GaugeReferenceProbe"])]:
            with (root / "input").open() as inputs, (root / name).open("w") as output:
                run(command, stdin=inputs, stdout=output)
        with (root / "input").open() as inputs, (root / "native").open() as native, (root / "reference").open() as reference:
            for index, triple in enumerate(itertools.zip_longest(inputs, native, reference), 1):
                case, actual, expected = triple
                if actual != expected:
                    mismatches += 1
                    if mismatches <= 10:
                        print(f"Mismatch row {index}: {case.strip() if case else '<missing>'}")
                        print(f"  native:    {actual.strip() if actual else '<missing>'}")
                        print(f"  reference: {expected.strip() if expected else '<missing>'}")
        print(f"Gauge differential: {count} cases, {mismatches} mismatches; reference {revision}")
        raise SystemExit(bool(mismatches))


if __name__ == "__main__":
    main()
