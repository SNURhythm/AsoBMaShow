#!/usr/bin/env python3
"""Verify Aso gameplay-skin output against the pinned Beatoraja oracle."""

from __future__ import annotations

import hashlib
import json
import math
import os
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
GENERATOR = ROOT / "scripts/generate_beatoraja_gameplay_skin_oracle.py"
HARNESS = ROOT / "tests/fixtures/beatoraja_skin/oracle/GameplaySkinOracle.java"
TRACE = ROOT / "tests/fixtures/beatoraja_skin/traces/gameplay_objects_pinned_v1.json"
LEDGER = ROOT / "docs/skin-compat/beatoraja-gameplay-feature-ledger-v1.json"
FIXTURES = (
    "tests/fixtures/beatoraja_skin/lua/model/all_v1_objects.luaskin",
    "tests/fixtures/beatoraja_skin/json/all_gameplay_fields.json",
    "tests/fixtures/beatoraja_skin/lr2/all_gameplay_objects.lr2skin",
    "tests/fixtures/beatoraja_skin/lr2/all_play_commands.lr2skin",
)
DIFFERENTIAL_TOKENS = (
    "bpmgraph",
    "bpmchart",
    "gauge-graph",
    "gaugegraph",
    "hit-error-visualizer",
    "hiterrorvisualizer",
    "judge-graph",
    "judgegraph",
    "notechart-1-p",
    "timing-1-p",
    "timing-distribution-graph",
    "timingdistributiongraph",
    "timing-visualizer",
    "timingvisualizer",
    "dst-slider",
    "src-slider",
    "slider-angle",
    "slider-changeable",
    "slider-range",
    "slider-type",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def differential_surface_ids() -> set[str]:
    rows = json.loads(LEDGER.read_text(encoding="utf-8"))["features"]
    return {
        row["id"]
        for row in rows
        if (any(token in row["id"] for token in DIFFERENTIAL_TOKENS)
            or row["id"] == "lua.property.float-property")
        and row["id"] != "lr2.csv-command.src-slider-refnumber"
    }


def compare_value(test: unittest.TestCase, source, aso, tolerance: float, path: str):
    if isinstance(source, dict):
        test.assertIsInstance(aso, dict, path)
        test.assertEqual(set(aso), set(source), path)
        for key in source:
            compare_value(test, source[key], aso[key], tolerance, f"{path}.{key}")
        return
    if isinstance(source, list):
        test.assertIsInstance(aso, list, path)
        test.assertEqual(len(aso), len(source), path)
        for index, expected in enumerate(source):
            compare_value(test, expected, aso[index], tolerance, f"{path}[{index}]")
        return
    if isinstance(source, float):
        test.assertIsInstance(aso, (int, float), path)
        test.assertNotIsInstance(aso, bool, path)
        test.assertTrue(math.isfinite(aso), path)
        test.assertAlmostEqual(aso, source, delta=tolerance, msg=path)
        return
    if isinstance(source, bool):
        test.assertIsInstance(aso, bool, path)
    elif isinstance(source, int):
        test.assertIsInstance(aso, int, path)
        test.assertNotIsInstance(aso, bool, path)
    test.assertEqual(aso, source, path)


class GameplaySkinOracleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.trace = json.loads(TRACE.read_text(encoding="utf-8")) if TRACE.is_file() else None

    def require_trace(self) -> dict:
        self.assertIsNotNone(self.trace, "pinned gameplay oracle trace must be committed")
        return self.trace

    def test_required_oracle_artifacts_are_committed(self):
        for path in (GENERATOR, HARNESS, TRACE):
            with self.subTest(path=path.relative_to(ROOT)):
                self.assertTrue(path.is_file(), f"missing oracle artifact: {path.relative_to(ROOT)}")

    def test_trace_envelope_pins_source_classpath_fixtures_and_frame(self):
        trace = self.require_trace()
        self.assertEqual(trace["schemaVersion"], 1)
        self.assertEqual(trace["referenceCommit"], PINNED_COMMIT)
        self.assertEqual(trace["oracle"], "pinned-beatoraja-gameplay-v1")
        self.assertEqual(trace["frame"], {
            "viewport": {"width": 1280, "height": 720},
            "visualTimesMillis": [0, 250, 500, 1500],
            "runtimeState": {
                "masterVolumeRate": 0.5,
                "recentHitErrorsMillis": [40, -20, 10],
            },
        })
        fixtures = {item["path"]: item["sha256"] for item in trace["fixtures"]}
        self.assertEqual(set(fixtures), set(FIXTURES))
        for relative in FIXTURES:
            self.assertEqual(fixtures[relative], sha256(ROOT / relative), relative)
        jars = trace["referenceClasspath"]["jars"]
        self.assertTrue(jars)
        self.assertEqual([item["path"] for item in jars], sorted(item["path"] for item in jars))
        for jar in jars:
            self.assertRegex(jar["sha256"], r"^[0-9a-f]{64}$")

    def test_every_differential_surface_has_exactly_one_source_case(self):
        trace = self.require_trace()
        expected = differential_surface_ids()
        self.assertEqual(set(trace["differentialSurfaceIds"]), expected)
        owners: dict[str, list[str]] = {identifier: [] for identifier in expected}
        case_ids = set()
        for case in trace["cases"]:
            self.assertNotIn(case["id"], case_ids)
            case_ids.add(case["id"])
            self.assertIn(case["comparison"]["mode"], {"exact", "absolute"})
            tolerance = case["comparison"]["tolerance"]
            self.assertIsInstance(tolerance, (int, float))
            self.assertGreaterEqual(tolerance, 0)
            self.assertTrue(case["source"])
            for identifier in case["surfaceIds"]:
                self.assertIn(identifier, owners)
                owners[identifier].append(case["id"])
        self.assertEqual(
            {identifier: cases for identifier, cases in owners.items() if len(cases) != 1},
            {},
        )

    def test_generator_check_is_byte_stable(self):
        result = subprocess.run(
            [sys.executable, str(GENERATOR), "--beatoraja-root", str(ROOT.parent / "beatoraja"), "--check"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_aso_trace_matches_pinned_source_with_declared_tolerances(self):
        trace = self.require_trace()
        executable = Path(os.environ.get(
            "ASOBMASHOW_GAMEPLAY_ORACLE_EXECUTABLE",
            ROOT / "cmake-build-debug/beatoraja_gameplay_cross_format_tests",
        ))
        result = subprocess.run(
            [str(executable), "--dump-gameplay-oracle"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        aso = json.loads(result.stdout)
        self.assertEqual(set(aso), {case["id"] for case in trace["cases"]})
        for case in trace["cases"]:
            tolerance = case["comparison"]["tolerance"]
            compare_value(self, case["source"], aso[case["id"]], tolerance, case["id"])


if __name__ == "__main__":
    unittest.main()
