#!/usr/bin/env python3
"""Regression tests for runner-owned music-select ledger evidence."""

import json
import os
import subprocess
import unittest
from pathlib import Path

from tests import beatoraja_music_select_skin_ledger_tests as ledger


TEST_RUNNERS = {
    "tests/gameplay_skin_traits_tests.cpp": "gameplay_skin_traits_tests",
}


def validate_executed_coverage(expected, emitted_by_runner):
    observed = {}
    for runner, identifiers in emitted_by_runner.items():
        assert len(identifiers) == len(set(identifiers)), (
            f"{runner} emitted duplicate ledger IDs"
        )
        for identifier in identifiers:
            assert identifier not in observed, (
                f"ledger ID emitted by multiple runners: {identifier}"
            )
            observed[identifier] = runner
    assert set(observed) == set(expected), (
        "executed ledger coverage differs: missing="
        + ",".join(sorted(set(expected) - set(observed)))
        + " extra="
        + ",".join(sorted(set(observed) - set(expected)))
    )
    assert all(observed[key] == expected[key] for key in expected), (
        "ledger IDs were emitted by the wrong runner"
    )


def executed_coverage(build_dir: Path, runners: set[str]):
    emitted = {}
    for runner in sorted(runners):
        executable = build_dir / runner
        assert executable.is_file(), f"ledger evidence is unbuilt: {runner}"
        completed = subprocess.run(
            [str(executable), "--list-ledger-assertions"],
            cwd=ledger.ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        assert completed.returncode == 0, completed.stdout
        payload = json.loads(completed.stdout.splitlines()[-1])
        assert payload.get("runner") == runner
        identifiers = payload.get("assertionIds")
        assert isinstance(identifiers, list)
        assert all(isinstance(identifier, str) and identifier
                   for identifier in identifiers)
        emitted[runner] = identifiers
    return emitted


class MusicSelectSkinLedgerEvidenceTests(unittest.TestCase):
    def test_implemented_rows_have_existing_owned_evidence(self):
        manifest = ledger.load_json(ledger.LEDGER_PATH)
        expected = {}
        for row in manifest["features"]:
            if row["status"] != "implemented":
                continue
            for field in ("implementation", "tests"):
                for value in row[field].split(";"):
                    self.assertTrue((ledger.ROOT / value.strip()).is_file(), row["id"])
            runner = row["assertion"]["runner"]
            self.assertIn(runner, TEST_RUNNERS.values(), row["id"])
            self.assertEqual(TEST_RUNNERS[row["tests"]], runner, row["id"])
            expected[row["id"]] = runner

        build_dir = Path(os.environ.get(
            "ASOBMASHOW_TEST_BUILD_DIR", ledger.ROOT / "cmake-build-debug"
        ))
        emitted = executed_coverage(build_dir, set(expected.values()))
        validate_executed_coverage(expected, emitted)

    def test_exact_coverage_rejects_missing_extra_duplicate_and_wrong_runner(self):
        expected = {"select.target.music-select": "gameplay_skin_traits_tests"}
        validate_executed_coverage(
            expected,
            {"gameplay_skin_traits_tests": ["select.target.music-select"]},
        )
        for emitted in (
            {"gameplay_skin_traits_tests": []},
            {"gameplay_skin_traits_tests": [
                "select.target.music-select", "select.target.extra"
            ]},
            {"gameplay_skin_traits_tests": [
                "select.target.music-select", "select.target.music-select"
            ]},
            {"another_runner": ["select.target.music-select"]},
        ):
            with self.subTest(emitted=emitted), self.assertRaises(AssertionError):
                validate_executed_coverage(expected, emitted)


if __name__ == "__main__":
    unittest.main()
