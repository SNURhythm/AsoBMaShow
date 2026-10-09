#!/usr/bin/env python3
"""Compare native gameplay runtime with the unchanged pinned Java JudgeManager.

The Java host doubles expose input/chart/state; judgement code is copied unchanged
from the verified reference. Gauge arithmetic is checked by the separate gauge
oracle, while this probe compares judgement counts, combo, mine hits and HCN ticks.
Requires the existing desktop build's parser object and localization library.
All probe compilation happens in a temporary directory, without invoking Ninja.
"""
import argparse
import hashlib
import itertools
import json
import os
import re
import random
from pathlib import Path
import shutil
import subprocess
import tempfile

PIN = "5233be081abee2a7f824b78aed0d783847deeb2f"
ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/lr2oraja_gameplay"


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def scenarios():
    dummy = (6, 0, 10_000_000, 0)
    # JudgeManager starts prevmtime at zero and passing uses (previous, current].
    # A negative first host update moves that lower edge before a zero-time note.
    for head, first_sample in itertools.product([-1_000, 0, 1], [None, -2_000, -1, 0]):
        press_time = max(0, head)
        prefix = [] if first_sample is None else [(0, 0, first_sample)]
        yield f"initial-hcn-boundary-{head}-{first_sample}", [(0, 3, head, 1_000_000), dummy], prefix + [(1, 0, press_time), (0, 0, 300_001), (0, 0, 600_002)]
        yield f"initial-mine-boundary-{head}-{first_sample}", [(0, 4, head, 0), dummy], prefix + [(1, 0, press_time), (0, 0, 300_001)]
    for head in [-1_000, 0, 1]:
        yield f"initial-hcn-negative-preupdate-{head}", [(0, 3, head, 1_000_000), dummy], [(0, 0, head - 1), (1, 0, head), (0, 0, 300_001)]
        yield f"initial-mine-negative-preupdate-{head}", [(0, 4, head, 0), dummy], [(0, 0, head - 1), (1, 0, head)]
    # Actions 4/5 stage clockwise press/release; 6/7 stage counterclockwise.
    # An explicit action 0 samples their latest states once, in Java key order.
    raw_patterns = [[4, 6, 5], [6, 4, 7], [4, 6], [6, 4], [4, 5],
                    [6, 7], [4, 5, 4], [6, 7, 6], [4, 6, 7], [6, 4, 5]]
    for kind, pattern_index, tail_clockwise, diff in itertools.product(
            [1, 2, 3], range(len(raw_patterns)), [False, True],
            [-500_000, -150_000, -100_000, 0, 100_000, 200_001]):
        pattern = raw_patterns[pattern_index]
        events = [(action, 7, 500_000) for action in pattern] + [(0, 7, 500_000)]
        held = [False, False]
        for action in pattern:
            held[0 if action <= 5 else 1] = action in [4, 6]
        tail_time = 1_500_000 + diff
        target = [tail_clockwise, not tail_clockwise]
        for direction in [0, 1]:
            if held[direction] != target[direction]:
                events.append(((4 if target[direction] else 5) + direction * 2, 7, tail_time))
        events += [(0, 7, tail_time), (5 if tail_clockwise else 7, 7, 1_750_001),
                   (0, 7, 1_750_001)]
        yield f"raw-scratch-{kind}-{pattern_index}-{tail_clockwise}-{diff}", [(7, kind, 500_000, 1_500_000), (7, 0, 1_550_000, 0), dummy], events
    for kind in [1, 2, 3]:
        yield f"raw-scratch-latest-owner-{kind}", [(7, kind, 500_000, 750_000), dummy], [(4, 7, 500_000), (6, 7, 500_000), (5, 7, 500_000), (0, 7, 500_000), (4, 7, 750_000), (0, 7, 750_000), (7, 7, 760_000), (0, 7, 760_000), (5, 7, 770_000), (0, 7, 770_000)]
    for kind, opposite, delay in itertools.product([1, 2, 3], [False, True], [0, 1, 100_000]):
        repress = 6 if opposite else 4
        yield f"raw-scratch-release-repress-{kind}-{opposite}-{delay}", [(7, kind, 500_000, 1_500_000), dummy], [(4, 7, 500_000), (0, 7, 500_000), (5, 7, 750_000), (repress, 7, 750_000), (0, 7, 750_000 + delay), (0, 7, 1_500_001)]
        yield f"raw-scratch-delayed-lift-{kind}-{opposite}-{delay}", [(7, kind, 500_000, 1_500_000), dummy], [(4, 7, 500_000), (0, 7, 500_000), (5, 7, 1_350_000), (0, 7, 1_350_000 + delay), (repress, 7, 1_450_001), (0, 7, 1_450_001), (0, 7, 1_750_001)]
    # The update step straddles the automatic-miss deadline by only one us.
    yield "input-before-miss", [(0, 0, 1_000_000, 0), (1, 0, 1_200_001, 0), dummy], [(0, 0, 1_200_000), (1, 1, 1_200_001)]
    for pressed, edge in itertools.product([False, True], [-1, 0, 1]):
        events = [] if pressed else [(1, 0, 999_998)]
        events += [(0, 0, 999_999), (1 if pressed else 2, 0, 1_000_000 + edge)]
        yield f"mine-boundary-{pressed}-{edge}", [(0, 4, 1_000_000, 0), dummy], events
    for kind, lane, head_diff, tail_diff in itertools.product(
            [1, 2, 3], [0, 7], [-200_000, -100_000, -18_000, 0, 18_000, 40_000, 100_000],
            [-300_001, -200_001, -200_000, -100_001, -100_000, -1, 0, 1, 100_000, 100_001, 200_000, 200_001, 300_001]):
        action = 3 if lane == 7 and kind != 1 else 2
        yield f"long-release-{kind}-{lane}-{head_diff}-{tail_diff}", [(lane, kind, 1_000_000, 2_000_000), dummy], [(1, lane, 1_000_000 + head_diff), (action, lane, 2_000_000 + tail_diff)]
    for kind in [1, 2, 3]:
        for time in [1_999_999, 2_000_000, 2_000_001, 2_200_000, 2_200_001]:
            yield f"held-tail-{kind}-{time}", [(0, kind, 1_000_000, 2_000_000), dummy], [(1, 0, 1_000_000), (0, 0, time)]
    for kind, diff in itertools.product([2, 3], [-200_001, -200_000, -100_001, -100_000, -1, 0, 1, 100_000, 100_001]):
        yield f"backspin-continuation-{kind}-{diff}", [(7, kind, 1_000_000, 2_000_000), (7, 0, 2_050_000, 0), dummy], [(1, 7, 1_000_000), (3, 7, 2_000_000 + diff)]
    for kind in [2, 3]:
        yield f"missed-long-head-{kind}", [(0, kind, 1_000_000, 2_000_000), dummy], [(0, 0, 1_200_001), (0, 0, 2_200_001)]
    for first, second in itertools.product([0, 1, 7], repeat=2):
        if first == second:
            continue
        yield f"simultaneous-miss-{first}-{second}", [(first, 0, 1_000_000, 0), (second, 0, 1_000_000, 0), dummy], [(0, 0, 1_200_001)]
    for step in [200_000, 200_001, 400_001, 700_000]:
        yield f"hcn-held-cadence-{step}", [(0, 3, 1_000_000, 5_000_000), dummy], [(1, 0, 1_000_000), (0, 0, 1_000_000 + step), (0, 0, 1_000_000 + 2 * step)]
        yield f"hcn-released-cadence-{step}", [(0, 3, 1_000_000, 5_000_000), dummy], [(1, 0, 1_000_000), (2, 0, 1_100_000), (0, 0, 1_100_000 + step), (0, 0, 1_100_000 + 2 * step)]
    yield "hcn-missed-head-later-hold", [(0, 3, 1_000_000, 5_000_000), dummy], [(0, 0, 1_200_001), (1, 0, 1_400_000), (0, 0, 1_700_000)]
    for survival in [0, 1]:
        yield f"survival-simultaneous-misses-{survival}", [(0, 0, 1_000_000, 0), (1, 0, 1_000_000, 0), dummy], [(0, 0, 1_200_001)], survival
        yield f"survival-miss-input-{survival}", [(0, 0, 1_000_000, 0), (1, 0, 1_200_001, 0), dummy], [(0, 0, 1_200_000), (1, 1, 1_200_001)], survival
        yield f"survival-simultaneous-mines-{survival}", [(0, 4, 1_000_000, 0), (1, 4, 1_000_000, 0), dummy], [(1, 0, 999_997), (1, 1, 999_998), (0, 0, 1_000_000)], survival
        yield f"survival-missed-charge-head-{survival}", [(0, 2, 1_000_000, 2_000_000), dummy], [(0, 0, 1_200_001)], survival
    for step in [200_000, 200_001, 400_001, 700_000]:
        yield f"mixed-held-hcn-mines-{step}", [(0, 3, 1_000_000, 5_000_000), (1, 3, 1_100_000, 5_000_000), (0, 4, 1_300_000, 0), (1, 4, 1_300_000, 0), dummy], [(1, 0, 1_000_000), (1, 1, 1_100_000), (0, 0, 1_300_000), (0, 0, 1_300_000 + step)]
        yield f"mixed-released-hcn-mines-{step}", [(0, 3, 1_000_000, 5_000_000), (1, 3, 1_100_000, 5_000_000), (0, 4, 1_300_000, 0), (1, 4, 1_300_000, 0), dummy], [(1, 0, 1_000_000), (1, 1, 1_100_000), (2, 0, 1_300_000), (0, 0, 1_300_000 + step)]

    rng = random.Random(5233)
    for case in range(300):
        notes = [(0, 3, 1_000_000, 5_000_000), (1, 2, 1_100_000, 3_600_000),
                 (0, 4, 1_500_000, 0), (0, 4, 2_800_000, 0),
                 (1, 4, 2_500_000, 0), (7, 4, 2_750_000, 0),
                 (7, 0, 1_350_000, 0), (7, 0, 2_050_000, 0),
                 (7, 0, 2_700_000, 0), (7, 0, 3_400_000, 0), dummy]
        edges = [(1, 0, 1_000_000), (1, 1, 1_100_000)]
        held = {0: True, 1: True, 7: False}
        time = 1_100_000
        while time < 5_400_000:
            time += rng.choice([1, 999, 100_000, 200_000, 200_001, 400_000, 700_000])
            if rng.random() < .25:
                edges.append((0, 0, time))
            else:
                lane = rng.choice([0, 1, 7])
                action = 2 if held[lane] else 1
                held[lane] = not held[lane]
                edges.append((action, lane, time))
        yield f"mixed-input-sequence-{case}", notes, edges



def encode(notes, edges, survival=0):
    return " ".join(map(str, [len(notes), survival, *(v for note in notes for v in note), len(edges), *(v for edge in edges for v in edge)])) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-repo", type=Path, default=ROOT.parent / "lr2oraja-endlessdream")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "cmake-build-debug")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    args = parser.parse_args()
    reference = args.reference_repo.resolve()
    commit = run(["git", "-C", str(reference), "rev-parse", "HEAD"], capture_output=True).stdout.strip()
    if commit != PIN:
        parser.error(f"Expected reference {PIN}, found {commit}")
    cases = list(scenarios())
    inputs = "".join(encode(*case[1:]) for case in cases)
    with tempfile.TemporaryDirectory(prefix="aso-runtime-parity-") as directory:
        build = Path(directory)
        source = build / "java-src"
        for name, content in json.loads((FIXTURES / "runtime_reference_support.json").read_text()).items():
            file = source / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(content)
        hashes = {}
        for name in ["JudgeManager.java", "JudgeProperty.java", "JudgeAlgorithm.java"]:
            relative = "core/src/bms/player/beatoraja/play/" + name
            file = reference / relative
            pinned = run(["git", "-C", str(reference), "show", PIN + ":" + relative], capture_output=True).stdout
            if file.read_text() != pinned:
                parser.error(f"Reference source differs from pinned commit: {relative}")
            hashes[name] = hashlib.sha256(file.read_bytes()).hexdigest()
            shutil.copy(file, source / "bms/player/beatoraja/play" / name)
        shutil.copy(FIXTURES / "runtime_reference_probe.java", source / "RuntimeReferenceProbe.java")
        run(["javac", "-d", str(build / "java-classes"), "-sourcepath", str(source), str(source / "RuntimeReferenceProbe.java")])
        # Reuse only unchanged parser and localization build outputs. Runtime,
        # candidate/window/gauge rules and chart definition are compiled fresh.
        target = re.search(r"^build gameplay_simulation_tests:.*$", (args.build_dir / "build.ninja").read_text(), re.MULTILINE)
        parser_object = re.search(r"CMakeFiles/[^ ]+/src/bms_parser.cpp.o", target.group(0)) if target else None
        if parser_object is None or not (args.build_dir / parser_object.group(0)).exists():
            parser.error("Missing parser test dependency object; build gameplay_simulation_tests first")
        include = args.build_dir / "vcpkg_installed/arm64-osx/include"
        sources = ["CompiledGameplayJudge", "GameplayCandidateRules", "GameplayGaugeRules", "GameplayJudgeRules", "GameplayNoteJudgeRole", "Judge", "GameplayDefinition", "GameplaySimulation", "SkinGameplayGraphState"]
        run([args.cxx, "-std=c++23", "-O1", "-I", str(ROOT / "src"), "-I", str(include), "-I", str(ROOT / "SDL/include"), str(FIXTURES / "runtime_probe.cpp"), *[str(ROOT / "src/scene/play" / (name + ".cpp")) for name in sources], str(args.build_dir / parser_object.group(0)), str(args.build_dir / "libasobmashow_localization.a"), "-o", str(build / "runtime-native")])
        java_output = run(["java", "-cp", str(build / "java-classes"), "RuntimeReferenceProbe"], input=inputs, capture_output=True).stdout.splitlines()
        diagnostics = [line for line in java_output if line == "ERROR: UNABLE TO FIND TNOTE"]
        java = [line for line in java_output if line != "ERROR: UNABLE TO FIND TNOTE"]
        native = run([str(build / "runtime-native")], input=inputs, capture_output=True).stdout.splitlines()
    if len(java) != len(cases) or len(native) != len(cases):
        raise SystemExit(f"Unexpected row count: cases={len(cases)}, Java={len(java)}, native={len(native)}")
    mismatches = [i for i, (expected, actual) in enumerate(zip(java, native)) if expected != actual]
    for index in dict.fromkeys(mismatches[:15] + mismatches[-10:]):
        print(f"case={cases[index][0]} Java={java[index]} native={native[index]} input={encode(*cases[index][1:]).strip()}")
    print(f"reference={commit}")
    for name, digest in hashes.items():
        print(f"{name}.sha256={digest}")
    print(f"Reference collector diagnostics={len(diagnostics)}")
    print(f"Runtime comparisons={len(cases)} mismatches={len(mismatches)}")
    return bool(mismatches)


if __name__ == "__main__":
    raise SystemExit(main())
