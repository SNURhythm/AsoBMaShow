#!/usr/bin/env python3
"""Regression tests for runner-owned gameplay-ledger evidence."""

import json
import subprocess
import unittest
from pathlib import Path

from tests import beatoraja_gameplay_skin_ledger_tests as ledger


class GameplaySkinLedgerEvidenceTests(unittest.TestCase):
    def test_native_runner_emits_test_owned_assertion_ids(self):
        executable = Path("cmake-build-debug/json_gameplay_skin_decoder_tests")
        self.assertTrue(executable.is_file())
        completed = subprocess.run(
            [str(executable), "--list-ledger-assertions"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout)
        evidence = json.loads(completed.stdout.splitlines()[-1])
        self.assertEqual(evidence["runner"], "json_gameplay_skin_decoder_tests")
        self.assertIn("json.field.animation-x", evidence["assertionIds"])
        self.assertEqual(
            evidence["assertionIds"], sorted(set(evidence["assertionIds"]))
        )

    def test_exact_executed_coverage_rejects_missing_extra_and_duplicate_ids(self):
        expected = {
            "json.field.animation-x": "json_gameplay_skin_decoder_tests",
            "lua.object-field.slider-range": "beatoraja_skin_model_tests",
        }
        ledger.validate_executed_coverage(
            expected,
            {
                "json_gameplay_skin_decoder_tests": ["json.field.animation-x"],
                "beatoraja_skin_model_tests": ["lua.object-field.slider-range"],
            },
        )
        for emitted in (
            {"json_gameplay_skin_decoder_tests": ["json.field.animation-x"]},
            {
                "json_gameplay_skin_decoder_tests": [
                    "json.field.animation-x",
                    "json.field.not-a-source-row",
                ],
                "beatoraja_skin_model_tests": ["lua.object-field.slider-range"],
            },
            {
                "json_gameplay_skin_decoder_tests": [
                    "json.field.animation-x",
                    "json.field.animation-x",
                ],
                "beatoraja_skin_model_tests": ["lua.object-field.slider-range"],
            },
        ):
            with self.subTest(emitted=emitted), self.assertRaises(AssertionError):
                ledger.validate_executed_coverage(expected, emitted)

    def test_exact_executed_coverage_rejects_a_row_emitted_by_the_wrong_runner(self):
        with self.assertRaises(AssertionError):
            ledger.validate_executed_coverage(
                {"json.field.animation-x": "json_gameplay_skin_decoder_tests"},
                {"beatoraja_skin_model_tests": ["json.field.animation-x"]},
            )


if __name__ == "__main__":
    unittest.main()
