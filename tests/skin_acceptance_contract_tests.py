#!/usr/bin/env python3
"""Contract tests for the external SCURO physical-acceptance verifier."""

from __future__ import annotations

import json
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "tests/fixtures/beatoraja_skin/reference_manifest.json"
VERIFIER = ROOT / "scripts/run_skin_acceptance.py"
PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"


class SkinAcceptanceContractTests(unittest.TestCase):
    def invoke(self, *arguments: str, cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(VERIFIER), *arguments],
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

    def test_default_contract_validation_is_clone_and_device_independent(self):
        result = self.invoke("validate", "--contract", str(CONTRACT))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("schema-v1 contract valid", result.stdout)

    def test_verify_refuses_committed_pending_physical_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = self.invoke(
                "verify",
                "--contract",
                str(CONTRACT),
                "--evidence-root",
                temporary,
                "--expected-app-commit",
                PINNED_COMMIT,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pending", result.stdout)

    def test_validate_rejects_unknown_contract_physical_evidence_fields(self):
        contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
        contract["acceptanceContract"]["physicalEvidence"]["deviceLabel"] = "private iPad"
        with tempfile.TemporaryDirectory() as temporary:
            contract_path = Path(temporary) / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            result = self.invoke("validate", "--contract", str(contract_path))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("physicalEvidence", result.stdout)

    def test_validate_rejects_mutated_frozen_negative_counter_contract(self):
        contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
        contract["acceptanceContract"]["negativeScenarios"][0]["deniedCountersExpected"]["filesystemWrites"] = "positive"
        with tempfile.TemporaryDirectory() as temporary:
            contract_path = Path(temporary) / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            result = self.invoke("validate", "--contract", str(contract_path))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("counter", result.stdout)

    def passing_contract(self) -> dict:
        contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
        acceptance = contract["acceptanceContract"]

        def mark_pass(value: object) -> None:
            if isinstance(value, dict):
                if "status" in value:
                    value["status"] = "pass"
                for child in value.values():
                    mark_pass(child)
            elif isinstance(value, list):
                for child in value:
                    mark_pass(child)

        mark_pass(acceptance)
        digest = "a" * 64
        acceptance["configuredHz"]["value"] = 60
        acceptance["hardwareModel"]["value"] = "iPad13,8"
        acceptance["iPadOS"]["value"] = "18.0"
        acceptance["drawableSize"]["value"] = {"height": 1080, "width": 1920}
        acceptance["safeInsets"]["value"] = {
            "bottom": 0,
            "left": 0,
            "right": 0,
            "top": 0,
        }
        acceptance["measurementBuild"]["value"] = {
            "commit": PINNED_COMMIT,
            "configuration": "Release",
            "sourceClean": True,
        }
        for item in acceptance["syntheticChartHashes"]:
            item["sha256"] = digest
        for item in acceptance["autoplayScripts"]:
            item["scriptSha256"] = digest
        for item in acceptance["completionCriteria"]:
            item["evidenceReference"] = "evidence-" + item["id"]
        for item in acceptance["layouts"]:
            item["evidenceReference"] = "screenshot-" + item["aspect"] + "-" + item["mode"]
        for item in acceptance["screenshotTimestamps"]:
            item["evidenceReference"] = "screenshot-" + item["aspect"] + "-" + item["mode"]
            item["timestampsMicros"] = [31_000_000]
        acceptance["timerEventTrace"].update(
            evidenceReference="evidence-timer-event", observedOrder=[1], selectedIds=[1]
        )
        acceptance["externalDigests"]["activatedRevisionSha256"]["value"] = digest
        acceptance["externalDigests"]["configurationSha256"]["value"] = digest
        for scenario in acceptance["negativeScenarios"]:
            scenario["overlayDigestBefore"] = digest
            scenario["overlayDigestAfter"] = digest
        acceptance["physicalEvidence"].update(
            status="pass",
            recordId="record-0123456789abcdef",
            accessControlledLocalEvidenceId="local-0123456789abcdef",
            redactionStatus="complete",
            retentionUntil="2030-01-01",
            deletionProcedure="documented-local-retention-procedure",
        )
        return contract

    def write_passing_evidence(self, root: Path, contract: dict, *, unsafe: dict | None = None) -> None:
        acceptance = contract["acceptanceContract"]
        digest = "a" * 64
        chart_hashes = {
            item["scenario"]: item["sha256"]
            for item in acceptance["syntheticChartHashes"]
        }
        autoplay_hashes = {
            item["scenario"]: item["scriptSha256"]
            for item in acceptance["autoplayScripts"]
        }
        layout_pairs = [
            (item["aspect"], item["mode"])
            for item in acceptance["screenshotTimestamps"]
        ]
        performance_runs = []
        for scenario_index, scenario in enumerate(chart_hashes):
            for layout_index, (aspect, mode) in enumerate(layout_pairs):
                for repetition in range(1, 4):
                    warmup_start = (
                        (scenario_index * len(layout_pairs) * 3 + layout_index * 3 + repetition)
                        * 1_000_000_000
                    )
                    performance_runs.append({
                        "scenario": scenario,
                        "aspect": aspect,
                        "mode": mode,
                        "repetition": repetition,
                        "chartSha256": chart_hashes[scenario],
                        "autoplayScriptSha256": autoplay_hashes[scenario],
                        "activatedRevisionSha256": digest,
                        "configurationSha256": digest,
                        "guardVectorSha256": acceptance["passingGuardVectorSha256"][0],
                        "warmupStartMicros": warmup_start,
                        "recordingStartMicros": warmup_start + 30_000_000,
                        "recordingEndMicros": warmup_start + 210_000_000,
                        "configuredRefreshHz": acceptance["configuredHz"]["value"],
                        "p99SkinCpuMicros": 1_000,
                        "missedPresentationPercent": 0.1,
                        "residentDriftBytes": 0,
                        "telemetry": {
                            "receivedSampleCount": 18_000,
                            "retainedSampleCount": 18_000,
                            "overflowSampleCount": 0,
                            "incompleteSampleCount": 0,
                            "mismatchedSampleCount": 0,
                        },
                        "renderIo": {
                            "filesystemReadsPerformed": 0,
                            "filesystemReadsDenied": 0,
                            "filesystemWritesPerformed": 0,
                            "filesystemWritesDenied": 0,
                            "filesystemDirectoryScansPerformed": 0,
                            "filesystemDirectoryScansDenied": 0,
                            "resourceUploadsPerformed": 0,
                            "resourceUploadsDenied": 0,
                        },
                    })
        evidence = {
            "schemaVersion": 1,
            "recordId": acceptance["physicalEvidence"]["recordId"],
            "accessControlledLocalEvidenceId": acceptance["physicalEvidence"][
                "accessControlledLocalEvidenceId"
            ],
            "redactionStatus": "complete",
            "retentionUntil": "2030-01-01",
            "deletionProcedure": "documented-local-retention-procedure",
            "completionEvidence": [
                {"id": item["evidenceReference"]}
                for item in acceptance["completionCriteria"]
            ],
            "screenshots": [
                {
                    "evidenceId": item["evidenceReference"],
                    "aspect": item["aspect"],
                    "mode": item["mode"],
                    "sha256": "b" * 64,
                    "width": 1920,
                    "height": 1080,
                    "timestampMicros": item["timestampsMicros"][0],
                }
                for item in acceptance["screenshotTimestamps"]
            ],
            "performanceRuns": performance_runs,
            "resourceLifecycle": {
                "baseline": {
                    "liveTextures": 0,
                    "liveResources": 0,
                    "residentBytes": 100,
                },
                "postDestruction": [
                    {
                        "cycle": cycle,
                        "liveTextures": 0,
                        "liveResources": 0,
                        "residentBytes": 100,
                    }
                    for cycle in range(1, 11)
                ],
            },
            "negativeScenario": {
                "scenarioId": acceptance["negativeScenarios"][0]["id"],
                "activatedRevisionSha256": digest,
                "configurationSha256": digest,
                "guardVectorSha256": acceptance["negativeScenarios"][0]["expectedGuardVectorSha256"],
                "diagnostic": "skin_file_render_phase_denied",
                "action": "discard_frame_disable_session_same_frame_builtin",
                "overlayDigestBefore": digest,
                "overlayDigestAfter": digest,
                "performedCounters": {
                    "filesystemReads": 0,
                    "filesystemWrites": 0,
                    "filesystemDirectoryScans": 0,
                    "resourceUploads": 0,
                },
                "deniedCounters": {
                    "filesystemReads": 1,
                    "filesystemWrites": 0,
                    "filesystemDirectoryScans": 0,
                    "resourceUploads": 0,
                },
            },
        }
        if unsafe:
            evidence.update(unsafe)
        (root / "acceptance-evidence.json").write_text(
            json.dumps(evidence), encoding="utf-8"
        )

    def test_verify_requires_external_metadata_and_rejects_unsafe_record(self):
        contract = self.passing_contract()
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract, unsafe={"publicUrl": "https://example.test"})
            result = self.invoke(
                "verify",
                "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("public URL", result.stdout)

    def test_verify_rejects_an_evidence_root_inside_the_repository(self):
        result = self.invoke(
            "verify",
            "--contract", str(CONTRACT),
            "--evidence-root", str(ROOT / "tests"),
            "--expected-app-commit", PINNED_COMMIT,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("inside repository", result.stdout)

    def test_verify_accepts_complete_metadata_only_evidence(self):
        contract = self.passing_contract()
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract)
            result = self.invoke(
                "verify",
                "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("physical SCURO acceptance evidence verified", result.stdout)

    def test_verify_rejects_duplicate_layout_screenshot_evidence_ids(self):
        contract = self.passing_contract()
        for layout, screenshot in zip(
            contract["acceptanceContract"]["layouts"],
            contract["acceptanceContract"]["screenshotTimestamps"],
            strict=True,
        ):
            layout["evidenceReference"] = "same-screenshot-evidence"
            screenshot["evidenceReference"] = "same-screenshot-evidence"
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract)
            result = self.invoke(
                "verify",
                "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unique", result.stdout)

    def test_verify_rejects_relative_path_as_deletion_procedure(self):
        contract = self.passing_contract()
        unsafe_procedure = "../../../Users/private/retention.txt"
        contract["acceptanceContract"]["physicalEvidence"]["deletionProcedure"] = unsafe_procedure
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract)
            evidence_path = temporary_root / "acceptance-evidence.json"
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            evidence["deletionProcedure"] = unsafe_procedure
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            result = self.invoke(
                "verify",
                "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("deletionProcedure", result.stdout)

    def test_verify_rejects_unequal_negative_overlay_digests(self):
        contract = self.passing_contract()
        contract["acceptanceContract"]["negativeScenarios"][0]["overlayDigestAfter"] = "b" * 64
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract)
            evidence_path = temporary_root / "acceptance-evidence.json"
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            evidence["negativeScenario"]["overlayDigestAfter"] = "b" * 64
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            result = self.invoke(
                "verify",
                "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlay", result.stdout)

    def test_verify_rejects_pending_negative_overlay_digest(self):
        contract = self.passing_contract()
        contract["acceptanceContract"]["negativeScenarios"][0]["overlayDigestAfter"] = "pending"
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract)
            result = self.invoke(
                "verify",
                "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlayDigestAfter", result.stdout)

    def test_verify_rejects_missing_retention_metadata(self):
        contract = self.passing_contract()
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract)
            evidence_path = temporary_root / "acceptance-evidence.json"
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            del evidence["retentionUntil"]
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            result = self.invoke(
                "verify",
                "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("retentionUntil", result.stdout)

    def test_verify_rejects_screenshot_payload_fields(self):
        contract = self.passing_contract()
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract)
            evidence_path = temporary_root / "acceptance-evidence.json"
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            evidence["screenshots"][0]["imagePayload"] = "not-allowed"
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            result = self.invoke(
                "verify",
                "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("screenshot records", result.stdout)

    def test_verify_rejects_public_urls_and_unbounded_personal_evidence_fields(self):
        contract = self.passing_contract()
        unsafe_records = (
            {"provenance": {"publicUrl": "https://example.test"}},
            {"localFile": "file:///Users/private/SCURO.png"},
            {"deviceLabel": "a user's iPad"},
            {"accountName": "private account"},
            {"udid": "private-device-id"},
            {"unknown": {"nested": "metadata"}},
        )
        for unsafe in unsafe_records:
            with self.subTest(unsafe=unsafe), tempfile.TemporaryDirectory() as temporary:
                temporary_root = Path(temporary)
                contract_path = temporary_root / "contract.json"
                contract_path.write_text(json.dumps(contract), encoding="utf-8")
                self.write_passing_evidence(temporary_root, contract, unsafe=unsafe)
                result = self.invoke(
                    "verify", "--contract", str(contract_path),
                    "--evidence-root", str(temporary_root),
                    "--expected-app-commit", PINNED_COMMIT,
                )
                self.assertNotEqual(result.returncode, 0)

    def test_verify_rejects_missing_or_incomplete_execution_reports(self):
        contract = self.passing_contract()
        for missing in ("performanceRuns", "resourceLifecycle", "negativeScenario"):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as temporary:
                temporary_root = Path(temporary)
                contract_path = temporary_root / "contract.json"
                contract_path.write_text(json.dumps(contract), encoding="utf-8")
                self.write_passing_evidence(temporary_root, contract)
                evidence_path = temporary_root / "acceptance-evidence.json"
                evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
                del evidence[missing]
                evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
                result = self.invoke(
                    "verify", "--contract", str(contract_path),
                    "--evidence-root", str(temporary_root),
                    "--expected-app-commit", PINNED_COMMIT,
                )
                self.assertNotEqual(result.returncode, 0)

    def test_schema_v1_requires_measured_lifecycle_resident_bytes(self):
        contract = self.passing_contract()
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract)
            evidence_path = temporary_root / "acceptance-evidence.json"
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            evidence["resourceLifecycle"]["baseline"].pop("residentBytes")
            for sample in evidence["resourceLifecycle"]["postDestruction"]:
                sample.pop("residentBytes")
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            result = self.invoke(
                "verify",
                "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("residentBytes", result.stdout)

    def test_verify_rejects_incomplete_or_limit_violating_execution_reports(self):
        contract = self.passing_contract()
        mutations = (
            lambda evidence: evidence["performanceRuns"].pop(),
            lambda evidence: evidence["performanceRuns"][0].update(p99SkinCpuMicros=1_000_000),
            lambda evidence: evidence["resourceLifecycle"]["postDestruction"].pop(),
            lambda evidence: evidence["negativeScenario"]["deniedCounters"].update(filesystemReads=0),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                temporary_root = Path(temporary)
                contract_path = temporary_root / "contract.json"
                contract_path.write_text(json.dumps(contract), encoding="utf-8")
                self.write_passing_evidence(temporary_root, contract)
                evidence_path = temporary_root / "acceptance-evidence.json"
                evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
                mutation(evidence)
                evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
                result = self.invoke(
                    "verify", "--contract", str(contract_path),
                    "--evidence-root", str(temporary_root),
                    "--expected-app-commit", PINNED_COMMIT,
                )
                self.assertNotEqual(result.returncode, 0)

    def test_verify_rejects_layout_mismatch_and_duplicate_completion_ids(self):
        contract = self.passing_contract()
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract)
            evidence_path = temporary_root / "acceptance-evidence.json"
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            contract["acceptanceContract"]["layouts"][0]["evidenceReference"] = "layout-not-screenshot"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            result = self.invoke(
                "verify", "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
            self.assertNotEqual(result.returncode, 0)
            contract = self.passing_contract()
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            evidence["completionEvidence"].append(evidence["completionEvidence"][0])
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            result = self.invoke(
                "verify", "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
            self.assertNotEqual(result.returncode, 0)

    def test_validate_rejects_tracked_file_matching_an_audited_payload_digest(self):
        spec = importlib.util.spec_from_file_location("skin_acceptance_verifier_test", VERIFIER)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(verifier)
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            subprocess.run(["git", "init", "-q", str(temporary_root)], check=True)
            (temporary_root / "external-payload.bin").write_bytes(b"")
            subprocess.run(["git", "-C", str(temporary_root), "add", "external-payload.bin"], check=True)
            with self.assertRaisesRegex(verifier.AcceptanceError, "audited SCURO payload digest"):
                verifier.tracked_payload_digest_check(
                    temporary_root,
                    {"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
                )


if __name__ == "__main__":
    unittest.main()
