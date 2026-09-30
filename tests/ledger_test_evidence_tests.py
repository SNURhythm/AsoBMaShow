#!/usr/bin/env python3
"""Behavioral checks for native ledger proof fixtures and standalone audits."""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tests.support import ledger_test_evidence as evidence


class LedgerTestEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.executable = self.directory / ("owner.exe" if os.name == "nt" else "owner")
        self.executable.write_text("native executable", encoding="utf-8")
        self.proofs = self.directory / "proofs"
        self.proofs.mkdir()
        self.proof = self.proofs / "owner.json"
        self.payload = {"runner": "owner", "assertionIds": ["owned.assertion"]}

    def record(self, output, returncode=0):
        completed = subprocess.CompletedProcess([], returncode, output)
        with mock.patch.object(evidence, "run_native", return_value=completed), \
                contextlib.redirect_stdout(io.StringIO()) as stdout:
            result = evidence.record_evidence(self.executable, "owner", self.proofs)
        self.assertEqual(stdout.getvalue(), output)
        return result

    def test_successful_proof_is_read_without_reexecuting_native(self):
        self.assertEqual(self.record("native output\n" + json.dumps(self.payload) + "\n"), 0)
        with mock.patch.dict(os.environ, {evidence.EVIDENCE_DIRECTORY_ENV: str(self.proofs)}), \
                mock.patch.object(evidence, "run_native") as run:
            self.assertEqual(
                evidence.executed_coverage(self.directory, {"owner"}),
                {"owner": ["owned.assertion"]},
            )
        run.assert_not_called()

    def test_failed_producer_removes_stale_proof_and_relays_failure(self):
        self.record(json.dumps(self.payload))
        self.assertTrue(self.proof.exists())
        self.assertEqual(self.record("native assertion failed\n", returncode=7), 7)
        self.assertFalse(self.proof.exists())

    def test_missing_invalid_or_wrong_producer_output_leaves_no_proof(self):
        for output in (
            "", "passed without JSON\n", "[]", "null",
            json.dumps({"runner": "other", "assertionIds": ["owned.assertion"]}),
            json.dumps({"runner": "owner", "assertionIds": []}),
            json.dumps({"runner": "owner", "assertionIds": ["a", "a"]}),
            json.dumps({"runner": "owner", "assertionIds": [2]}),
        ):
            with self.subTest(output=output):
                self.proof.write_text("stale", encoding="utf-8")
                with self.assertRaises(AssertionError):
                    self.record(output)
                self.assertFalse(self.proof.exists())

    def test_launch_failure_removes_stale_proof(self):
        self.proof.write_text("stale", encoding="utf-8")
        with mock.patch.object(evidence, "run_native", side_effect=OSError("cannot launch")):
            with self.assertRaises(OSError):
                evidence.record_evidence(self.executable, "owner", self.proofs)
        self.assertFalse(self.proof.exists())

    def test_audit_rejects_missing_invalid_and_stale_proof_without_fallback(self):
        self.record(json.dumps(self.payload))
        valid = self.proof.read_text(encoding="utf-8")
        with mock.patch.dict(os.environ, {evidence.EVIDENCE_DIRECTORY_ENV: str(self.proofs)}), \
                mock.patch.object(evidence, "run_native") as run:
            for content in (None, "invalid", "{}", "null"):
                with self.subTest(content=content):
                    self.proof.unlink(missing_ok=True)
                    if content is not None:
                        self.proof.write_text(content, encoding="utf-8")
                    with self.assertRaises(AssertionError):
                        evidence.executed_coverage(self.directory, {"owner"})
            self.proof.write_text(valid, encoding="utf-8")
            self.executable.write_text("a different executable", encoding="utf-8")
            with self.assertRaisesRegex(AssertionError, "stale"):
                evidence.executed_coverage(self.directory, {"owner"})
        run.assert_not_called()

    def test_standalone_audit_still_executes_native(self):
        completed = subprocess.CompletedProcess([], 0, json.dumps(self.payload))
        with mock.patch.dict(os.environ):
            os.environ.pop(evidence.EVIDENCE_DIRECTORY_ENV, None)
            with mock.patch.object(evidence, "run_native", return_value=completed) as run:
                self.assertEqual(
                    evidence.executed_coverage(self.directory, {"owner"}),
                    {"owner": ["owned.assertion"]},
                )
            run.assert_called_once_with(self.executable)

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"),
                         "fixture configuration requires CMake/CTest")
    def test_missing_owner_disables_only_complete_audit(self):
        module = evidence.ROOT / "cmake/LedgerTestEvidence.cmake"
        (self.directory / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.22)\n"
            "project(MissingLedgerOwner NONE)\n"
            "enable_testing()\n"
            f'set(Python3_EXECUTABLE "{Path(sys.executable).as_posix()}")\n'
            f'include("{module.as_posix()}")\n'
            'add_test(NAME audit COMMAND "${Python3_EXECUTABLE}" -c "raise RuntimeError()")\n'
            'add_test(NAME unrelated COMMAND "${Python3_EXECUTABLE}" -c "print(123)")\n'
            "asobmashow_require_ledger_evidence(audit unavailable_owner)\n",
            encoding="utf-8",
        )
        build = self.directory / "build"
        configured = subprocess.run(
            ["cmake", "-S", str(self.directory), "-B", str(build)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        self.assertEqual(configured.returncode, 0, configured.stdout)
        self.assertIn("unavailable_owner", configured.stdout)
        listing = subprocess.run(
            ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
            text=True, capture_output=True, check=True,
        )
        audit = next(test for test in json.loads(listing.stdout)["tests"] if test["name"] == "audit")
        properties = {item["name"]: item["value"] for item in audit["properties"]}
        self.assertTrue(properties["DISABLED"])
        self.assertNotIn("FIXTURES_REQUIRED", properties)
        self.assertNotIn("ENVIRONMENT", properties)
        completed = subprocess.run(
            ["ctest", "--test-dir", str(build), "--output-on-failure"],
            text=True, capture_output=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertIn("unrelated", completed.stdout)
        self.assertIn("Disabled", completed.stdout)

    @unittest.skipUnless(os.name != "nt" and shutil.which("cmake") and shutil.which("ctest"),
                         "fixture smoke test requires CMake/CTest and POSIX executable scripts")
    def test_filtered_ctest_reruns_producer_and_failed_producer_skips_audit(self):
        # This tiny project exercises real CTest fixture scheduling, without
        # building or running any application target.
        self.executable.write_text(
            f"#!{sys.executable}\n"
            "import json, pathlib, sys\n"
            "root = pathlib.Path(__file__).parent\n"
            "with (root / 'native-runs').open('a') as stream: stream.write('run\\n')\n"
            "assert sys.argv[1:] == ['--list-ledger-assertions']\n"
            "if (root / 'fail').exists(): sys.exit(7)\n"
            f"print({json.dumps(self.payload)!r})\n",
            encoding="utf-8",
        )
        self.executable.chmod(0o755)
        audit = self.directory / "audit.py"
        audit.write_text(
            "import pathlib, sys\n"
            f"sys.path.insert(0, {str(evidence.ROOT)!r})\n"
            "from tests.support.ledger_test_evidence import executed_coverage\n"
            "root = pathlib.Path(__file__).parent\n"
            "assert executed_coverage(root, {'owner'}) == {'owner': ['owned.assertion']}\n"
            "with (root / 'audit-runs').open('a') as stream: stream.write('run\\n')\n",
            encoding="utf-8",
        )
        module = evidence.ROOT / "cmake/LedgerTestEvidence.cmake"
        (self.directory / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.22)\n"
            "project(LedgerFixture NONE)\n"
            "enable_testing()\n"
            f'set(Python3_EXECUTABLE "{Path(sys.executable).as_posix()}")\n'
            f'include("{module.as_posix()}")\n'
            "add_executable(owner IMPORTED)\n"
            f'set_property(TARGET owner PROPERTY IMPORTED_LOCATION "{self.executable.as_posix()}")\n'
            "asobmashow_add_ledger_test(owner native_alias)\n"
            f'add_test(NAME audit COMMAND "${{Python3_EXECUTABLE}}" "{audit.as_posix()}")\n'
            "asobmashow_require_ledger_evidence(audit owner)\n",
            encoding="utf-8",
        )
        build = self.directory / "build"
        configured = subprocess.run(
            ["cmake", "-S", str(self.directory), "-B", str(build), "-G", "Unix Makefiles"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        self.assertEqual(configured.returncode, 0, configured.stdout)
        command = ["ctest", "--test-dir", str(build), "-R", "^audit$", "--output-on-failure"]
        for expected_runs in (1, 2):
            completed = subprocess.run(command, text=True, capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            for name in ("native-runs", "audit-runs"):
                self.assertEqual((self.directory / name).read_text().splitlines(), ["run"] * expected_runs)
        (self.directory / "fail").touch()
        completed = subprocess.run(command, text=True, capture_output=True)
        self.assertNotEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("Not Run", completed.stdout)
        self.assertEqual((self.directory / "native-runs").read_text().splitlines(), ["run"] * 3)
        self.assertEqual((self.directory / "audit-runs").read_text().splitlines(), ["run"] * 2)
        self.assertFalse((build / "ledger-evidence/owner.json").exists())


if __name__ == "__main__":
    unittest.main()
