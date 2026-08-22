#!/usr/bin/env python3
"""Contract tests for the external SCURO physical-acceptance verifier."""

from __future__ import annotations

import json
import importlib.util
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "tests/fixtures/beatoraja_skin/reference_manifest.json"
VERIFIER = ROOT / "scripts/run_skin_acceptance.py"
MODERNCHIC_VERIFIER = ROOT / "scripts/verify_modernchic_gameplay_skin.py"
SCURO_TRACE = ROOT / "tests/fixtures/beatoraja_skin/traces/scuro_property_frames_v1.json"
PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"


def iso_bmff_boxes(payload: bytes, start: int = 0, end: int | None = None):
    """Yield strict (type, payload-start, end) tuples for one ISO-BMFF level."""
    boundary = len(payload) if end is None else end
    cursor = start
    while cursor < boundary:
        if boundary - cursor < 8:
            raise ValueError("truncated ISO-BMFF box header")
        size, box_type = struct.unpack_from(">I4s", payload, cursor)
        header_size = 8
        if size == 1:
            if boundary - cursor < 16:
                raise ValueError("truncated ISO-BMFF extended box header")
            size = struct.unpack_from(">Q", payload, cursor + 8)[0]
            header_size = 16
        elif size == 0:
            size = boundary - cursor
        if size < header_size or cursor + size > boundary:
            raise ValueError(f"invalid ISO-BMFF box size for {box_type!r}")
        yield box_type, cursor + header_size, cursor + size
        cursor += size
    if cursor != boundary:
        raise ValueError("ISO-BMFF boxes do not fill their parent")


def required_iso_bmff_box(payload: bytes, start: int, end: int, wanted: bytes):
    matches = [box for box in iso_bmff_boxes(payload, start, end) if box[0] == wanted]
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {wanted!r} box, found {len(matches)}")
    return matches[0]


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

    def modernchic_verifier(self):
        self.assertTrue(
            MODERNCHIC_VERIFIER.is_file(),
            "the complete noncommitting ModernChic verifier must exist",
        )
        spec = importlib.util.spec_from_file_location(
            "verify_modernchic_gameplay_skin", MODERNCHIC_VERIFIER
        )
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        verifier = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = verifier
        self.addCleanup(sys.modules.pop, spec.name, None)
        spec.loader.exec_module(verifier)
        return verifier

    @staticmethod
    def passing_modernchic_session_report() -> dict:
        return {
            "schemaVersion": 1,
            "entryIdentity": "entry-d5399e62255ddbda273e7a63",
            "sessionPublished": True,
            "graphFamilies": {
                name: {"declared": 1, "commands": 1}
                for name in (
                    "noteDistribution",
                    "timingVisualizer",
                    "bpmGraph",
                    "hitErrorVisualizer",
                )
            },
            "resourcePreparation": {
                "complete": True,
                "allReferencedResourcesPrepared": True,
                "imageDecodes": 4,
                "fontDecodes": 1,
                "movieDecodes": 0,
                "audioDecodes": 0,
                "textureUploads": 5,
            },
            "callbackBudget": {
                "frameBudgetMicros": 6_000,
                "framesEvaluated": 3,
                "maximumFrameWallMicros": 5_000,
                "violationDiagnostics": [],
            },
            "unsupportedDiagnostics": [],
            "unsupportedSubjects": [],
            "diagnosticCodes": [],
            "loading": {
                "complete": True,
                "totalMicros": 123_456,
                "measuredMicros": 123_500,
            },
        }

    def test_modernchic_verifier_requires_every_runtime_acceptance_fact(self):
        verifier = self.modernchic_verifier()
        trace = json.loads(SCURO_TRACE.read_text(encoding="utf-8"))
        passing = self.passing_modernchic_session_report()
        verifier.validate_session_report(
            passing, trace, "entry-d5399e62255ddbda273e7a63"
        )

        mutations = (
            lambda report: report["graphFamilies"].pop("bpmGraph"),
            lambda report: report["graphFamilies"]["timingVisualizer"].update(commands=0),
            lambda report: report["resourcePreparation"].update(
                allReferencedResourcesPrepared=False
            ),
            lambda report: report["callbackBudget"]["violationDiagnostics"].append(
                "skin_lua_wall_time_limit_exceeded"
            ),
            lambda report: report["diagnosticCodes"].append(
                "skin_lua_execution_failed"
            ),
            lambda report: report["unsupportedDiagnostics"].append(
                "skin.renderer.unsupported"
            ),
            lambda report: report["loading"].update(complete=False),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                malformed = json.loads(json.dumps(passing))
                mutation(malformed)
                with self.assertRaises(verifier.VerificationError):
                    verifier.validate_session_report(
                        malformed, trace, "entry-d5399e62255ddbda273e7a63"
                    )

    def test_scuro_trace_pins_beatoraja_long_note_slots_not_a_cyclic_shift(self):
        verifier = self.modernchic_verifier()
        trace = json.loads(SCURO_TRACE.read_text(encoding="utf-8"))
        verifier.validate_long_note_trace(trace)
        shifted = json.loads(json.dumps(trace))
        cases = shifted["longNoteSelectorOracle"]["undefinedByModelType"]
        modes = [case["mode"] for case in cases]
        for index, case in enumerate(cases):
            case["mode"] = modes[(index + 1) % len(modes)]
        with self.assertRaises(verifier.VerificationError):
            verifier.validate_long_note_trace(shifted)

    def test_default_contract_validation_is_clone_and_device_independent(self):
        result = self.invoke("validate", "--contract", str(CONTRACT))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("schema-v2 acceptance contract valid", result.stdout)

    def schema_v2_contract(self) -> dict:
        contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
        acceptance = contract["acceptanceContract"]
        acceptance["schemaVersion"] = 2
        for legacy_key in ("negativeScenarios", "passingGuardVectorSha256"):
            acceptance.pop(legacy_key, None)
        acceptance["ordinaryRuntimeIo"] = {
            "status": "pending",
            "configuredLoadOperations": [],
            "renderCallbackOperations": [],
            "evidenceReference": None,
        }
        acceptance["limits"] = {
            "liveResourceGrowthAfterTenExits": 0,
            "missedPresentationPercent": 0.5,
            "p99SkinCpuFrameFraction": 0.9,
            "residentMemoryDriftMiB": 32,
        }
        return contract

    def test_schema_v2_accepts_observed_ordinary_selected_root_io(self):
        contract = self.schema_v2_contract()
        contract["acceptanceContract"]["ordinaryRuntimeIo"].update(
            status="pass",
            configuredLoadOperations=[
                "filesystemRead",
                "filesystemWrite",
                "filesystemDirectoryScan",
            ],
            renderCallbackOperations=["filesystemRead", "filesystemWrite"],
            evidenceReference="ordinary-runtime-io",
        )
        with tempfile.TemporaryDirectory() as temporary:
            contract_path = Path(temporary) / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            result = self.invoke("validate", "--contract", str(contract_path))
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_schema_v2_rejects_legacy_render_io_restriction_fields(self):
        legacy_fields = {
            "negativeScenarios": [],
            "passingGuardVectorSha256": ["a" * 64],
            "overlayDigestBefore": "a" * 64,
            "overlayDigestAfter": "a" * 64,
            "deniedCountersExpected": {"filesystemReads": 1},
        }
        for field, value in legacy_fields.items():
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                contract = self.schema_v2_contract()
                contract["acceptanceContract"][field] = value
                contract_path = Path(temporary) / "contract.json"
                contract_path.write_text(json.dumps(contract), encoding="utf-8")
                result = self.invoke("validate", "--contract", str(contract_path))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("legacy schema-v1 field", result.stdout)

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

    def test_validate_rejects_legacy_negative_counter_contract(self):
        contract = self.schema_v2_contract()
        contract["acceptanceContract"]["deniedCountersExpected"] = {"filesystemWrites": "positive"}
        with tempfile.TemporaryDirectory() as temporary:
            contract_path = Path(temporary) / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            result = self.invoke("validate", "--contract", str(contract_path))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("legacy schema-v1 field", result.stdout)

    def test_redistributable_fixture_digests_are_exact_and_required(self):
        contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
        expected = [
            {"path": "charts/acceptance_7k.bms", "sha256": "0060de67ef50532e3747c7ac486045a8ef0c192d34853b1f424d4bc650406f00"},
            {"path": "charts/acceptance_bga_base.png", "sha256": "54102735089d1f3d5a8838915def4bb18e704e9ecac889c2c8c65ee8e60bbc31"},
            {"path": "charts/acceptance_bga_layer.png", "sha256": "fc03e493884867abaf863f48ad09fd5ebe3aefd14221ea9ade7a69c361d0615f"},
            {"path": "charts/acceptance_bga_miss.png", "sha256": "adfa0c7de03bc3bea3de80b4a4514881c8b6296568f43a5acd5cd7a16fffd1c9"},
            {"path": "charts/acceptance_bga_video.mp4", "sha256": "e947526c2c7e26632b09c4eb3f1ce912bd18094c7f224c0ef09138d70e361946"},
        ]
        self.assertEqual(contract["redistributableSyntheticFixtureDigests"], expected)
        audit_spec = importlib.util.spec_from_file_location(
            "beatoraja_fixture_digest_audit", ROOT / "scripts/audit_beatoraja_skin.py"
        )
        self.assertIsNotNone(audit_spec)
        self.assertIsNotNone(audit_spec.loader)
        audit = importlib.util.module_from_spec(audit_spec)
        sys.modules[audit_spec.name] = audit
        self.addCleanup(sys.modules.pop, audit_spec.name, None)
        audit_spec.loader.exec_module(audit)
        self.assertEqual(audit.redistributable_synthetic_fixture_digests(), expected)

        malformed_contracts = []
        missing = json.loads(json.dumps(contract))
        del missing["redistributableSyntheticFixtureDigests"]
        malformed_contracts.append(missing)
        duplicate = json.loads(json.dumps(contract))
        duplicate["redistributableSyntheticFixtureDigests"][1]["path"] = (
            "charts/acceptance_7k.bms"
        )
        malformed_contracts.append(duplicate)
        changed = json.loads(json.dumps(contract))
        changed["redistributableSyntheticFixtureDigests"][0]["sha256"] = "0" * 64
        malformed_contracts.append(changed)
        with tempfile.TemporaryDirectory() as temporary:
            contract_path = Path(temporary) / "contract.json"
            for malformed in malformed_contracts:
                contract_path.write_text(json.dumps(malformed), encoding="utf-8")
                result = self.invoke("validate", "--contract", str(contract_path))
                self.assertNotEqual(result.returncode, 0, result.stdout)

    def passing_contract(self) -> dict:
        contract = self.schema_v2_contract()
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
        acceptance["ordinaryRuntimeIo"].update(
            configuredLoadOperations=[
                "filesystemRead",
                "filesystemWrite",
                "filesystemDirectoryScan",
            ],
            renderCallbackOperations=["filesystemRead", "filesystemWrite"],
            evidenceReference="ordinary-runtime-io",
        )
        acceptance["externalDigests"]["activatedRevisionSha256"]["value"] = digest
        acceptance["externalDigests"]["configurationSha256"]["value"] = digest
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
                    })
        evidence = {
            "schemaVersion": 2,
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
            "ordinaryRuntimeIo": {
                "configuredLoadOperations": acceptance["ordinaryRuntimeIo"]["configuredLoadOperations"],
                "renderCallbackOperations": acceptance["ordinaryRuntimeIo"]["renderCallbackOperations"],
                "evidenceReference": acceptance["ordinaryRuntimeIo"]["evidenceReference"],
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

    def test_verify_rejects_mismatched_ordinary_runtime_io_observations(self):
        contract = self.passing_contract()
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract)
            evidence_path = temporary_root / "acceptance-evidence.json"
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            evidence["ordinaryRuntimeIo"]["renderCallbackOperations"] = ["filesystemRead"]
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            result = self.invoke(
                "verify",
                "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ordinary runtime I/O", result.stdout)

    def test_verify_rejects_noncanonical_ordinary_runtime_io_observations(self):
        contract = self.passing_contract()
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            contract_path = temporary_root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            self.write_passing_evidence(temporary_root, contract)
            evidence_path = temporary_root / "acceptance-evidence.json"
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            evidence["ordinaryRuntimeIo"]["configuredLoadOperations"] = [
                "filesystemWrite", "filesystemRead",
            ]
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            result = self.invoke(
                "verify",
                "--contract", str(contract_path),
                "--evidence-root", str(temporary_root),
                "--expected-app-commit", PINNED_COMMIT,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("canonical operation order", result.stdout)

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
        for missing in ("performanceRuns", "resourceLifecycle", "ordinaryRuntimeIo"):
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

    def test_schema_v2_requires_measured_lifecycle_resident_bytes(self):
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
            lambda evidence: evidence["ordinaryRuntimeIo"].update(
                renderCallbackOperations=[]
            ),
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

    def test_redistributable_acceptance_chart_fixture_is_self_describing(self):
        """Keep the local synthetic chart and BGA inputs runnable without SCURO payloads."""
        fixture_root = ROOT / "tests/fixtures/beatoraja_skin/charts"
        chart = fixture_root / "acceptance_7k.bms"
        readme = fixture_root / "README.md"
        chart_bytes = chart.read_bytes()
        chart_text = chart_bytes.decode("ascii")
        readme_text = readme.read_text(encoding="utf-8")
        normalized_readme = " ".join(readme_text.split())
        self.assertIn("#LNTYPE 1", chart_text)
        self.assertNotIn("#LNTYPE 2", chart_text)
        self.assertNotIn("#LNMODE", chart_text)
        self.assertIn("#002D1:", chart_text)
        self.assertIn("#00206:02000300", chart_text)
        self.assertIn("landmine", readme_text.lower())
        self.assertIn("transparent gap", readme_text.lower())
        for mode in ("LN", "CN", "HCN"):
            self.assertIn(f"separate {mode} runtime/autoplay run", normalized_readme)

        expected_hashes = {
            "README.md": "235903883f5be4cc67db382c17c128d4d851e9c31383259f538ef7172786bb1d",
            "acceptance_7k.bms": "0060de67ef50532e3747c7ac486045a8ef0c192d34853b1f424d4bc650406f00",
            "acceptance_bga_base.png": "54102735089d1f3d5a8838915def4bb18e704e9ecac889c2c8c65ee8e60bbc31",
            "acceptance_bga_layer.png": "fc03e493884867abaf863f48ad09fd5ebe3aefd14221ea9ade7a69c361d0615f",
            "acceptance_bga_miss.png": "adfa0c7de03bc3bea3de80b4a4514881c8b6296568f43a5acd5cd7a16fffd1c9",
            "acceptance_bga_video.mp4": "e947526c2c7e26632b09c4eb3f1ce912bd18094c7f224c0ef09138d70e361946",
        }
        for filename, expected_hash in expected_hashes.items():
            with self.subTest(filename=filename):
                payload = (fixture_root / filename).read_bytes()
                self.assertEqual(hashlib.sha256(payload).hexdigest(), expected_hash)

        for required_line in (
            "#PLAYER 1",
            "#PLAYLEVEL 7",
            "#BPM 120",
            "#BMP01 acceptance_bga_base.png",
            "#BMP02 acceptance_bga_layer.png",
            "#BMP03 acceptance_bga_miss.png",
            "#BMP04 acceptance_bga_video.mp4",
            "#LNOBJ ZZ",
            "#LNTYPE 1",
            "#00003:",
            "#00008:",
            "#00009:",
            "#000SC:",
            "#00004:",
            "#00111:",
            "#00151:",
            "#002D1:",
            "#00206:02000300",
            "#00404:",
            "#00411:",
        ):
            self.assertIn(required_line, chart_text)

        expected_png_structure = {
            "acceptance_bga_base.png": ([b"IHDR", b"PLTE", b"IDAT", b"IEND"], 1, 3),
            "acceptance_bga_layer.png": ([b"IHDR", b"PLTE", b"IDAT", b"IEND"], 1, 3),
            "acceptance_bga_miss.png": ([b"IHDR", b"IDAT", b"IEND"], 8, 6),
        }
        for filename, (expected_chunks, expected_bit_depth, expected_color_type) in expected_png_structure.items():
            payload = (fixture_root / filename).read_bytes()
            self.assertEqual(payload[:8], b"\x89PNG\r\n\x1a\n", filename)
            chunks = []
            idat_payload = bytearray()
            cursor = 8
            while cursor < len(payload):
                self.assertGreaterEqual(len(payload) - cursor, 12, filename)
                chunk_length = struct.unpack_from(">I", payload, cursor)[0]
                chunk_type = payload[cursor + 4:cursor + 8]
                chunk_end = cursor + 12 + chunk_length
                self.assertLessEqual(chunk_end, len(payload), filename)
                chunk_data = payload[cursor + 8:cursor + 8 + chunk_length]
                expected_crc = struct.unpack_from(">I", payload, cursor + 8 + chunk_length)[0]
                actual_crc = zlib.crc32(chunk_type + chunk_data) & 0xFFFFFFFF
                self.assertEqual(actual_crc, expected_crc, filename)
                chunks.append(chunk_type)
                if chunk_type == b"IHDR":
                    width, height, bit_depth, color_type, compression, filtering, interlace = (
                        struct.unpack(">IIBBBBB", chunk_data)
                    )
                    self.assertEqual((width, height), (2, 2), filename)
                    self.assertEqual((bit_depth, color_type), (expected_bit_depth, expected_color_type), filename)
                    self.assertEqual((compression, filtering, interlace), (0, 0, 0), filename)
                elif chunk_type == b"IDAT":
                    idat_payload.extend(chunk_data)
                cursor = chunk_end
            self.assertEqual(chunks, expected_chunks, filename)
            if filename == "acceptance_bga_miss.png":
                scanlines = zlib.decompress(idat_payload)
                self.assertEqual(len(scanlines), 18)
                self.assertEqual((scanlines[0], scanlines[9]), (0, 0), "gap PNG uses unfiltered rows")
                self.assertEqual(
                    [scanlines[index] for index in (4, 8, 13, 17)],
                    [0, 0, 0, 0],
                    "every gap-card pixel must be transparent",
                )

        mp4 = (fixture_root / "acceptance_bga_video.mp4").read_bytes()
        top_level = list(iso_bmff_boxes(mp4))
        self.assertEqual(top_level[0][0], b"ftyp")
        _, moov_start, moov_end = required_iso_bmff_box(mp4, 0, len(mp4), b"moov")
        _, mdat_start, mdat_end = required_iso_bmff_box(mp4, 0, len(mp4), b"mdat")
        tracks = [box for box in iso_bmff_boxes(mp4, moov_start, moov_end) if box[0] == b"trak"]
        self.assertEqual(len(tracks), 1)
        _, track_start, track_end = tracks[0]
        _, mdia_start, mdia_end = required_iso_bmff_box(mp4, track_start, track_end, b"mdia")
        _, handler_start, handler_end = required_iso_bmff_box(mp4, mdia_start, mdia_end, b"hdlr")
        self.assertEqual(mp4[handler_start + 8:handler_start + 12], b"vide")
        self.assertEqual(mp4[handler_start + 24:handler_end].rstrip(b"\x00"), b"")
        _, mdhd_start, mdhd_end = required_iso_bmff_box(mp4, mdia_start, mdia_end, b"mdhd")
        self.assertEqual(mp4[mdhd_start], 0, "fixture uses compact version-0 media header")
        timescale, duration = struct.unpack_from(">II", mp4, mdhd_start + 12)
        self.assertEqual(duration, timescale, "fixture duration must be exactly one second")

        _, minf_start, minf_end = required_iso_bmff_box(mp4, mdia_start, mdia_end, b"minf")
        _, stbl_start, stbl_end = required_iso_bmff_box(mp4, minf_start, minf_end, b"stbl")
        _, stsd_start, stsd_end = required_iso_bmff_box(mp4, stbl_start, stbl_end, b"stsd")
        self.assertEqual(struct.unpack_from(">I", mp4, stsd_start + 4)[0], 1)
        sample_entry = stsd_start + 8
        sample_entry_size, codec = struct.unpack_from(">I4s", mp4, sample_entry)
        self.assertLessEqual(sample_entry + sample_entry_size, stsd_end)
        self.assertEqual(codec, b"avc1")
        self.assertEqual(struct.unpack_from(">HH", mp4, sample_entry + 32), (2, 2))
        _, stsz_start, _ = required_iso_bmff_box(mp4, stbl_start, stbl_end, b"stsz")
        self.assertEqual(struct.unpack_from(">I", mp4, stsz_start + 8)[0], 1)

        nal_types = []
        cursor = mdat_start
        while cursor < mdat_end:
            self.assertGreaterEqual(mdat_end - cursor, 4, "truncated AVC NAL length")
            nal_size = struct.unpack_from(">I", mp4, cursor)[0]
            cursor += 4
            self.assertGreater(nal_size, 0)
            self.assertLessEqual(cursor + nal_size, mdat_end, "truncated AVC NAL payload")
            nal_types.append(mp4[cursor] & 0x1F)
            cursor += nal_size
        self.assertIn(5, nal_types, "one-frame fixture must contain an IDR frame")
        self.assertNotIn(6, nal_types, "fixture must not contain H.264 SEI/user data")

        lower_mp4 = mp4.lower()
        for forbidden in (b"udta", b"meta", b"lavf", b"lavc", b"x264", b"videolan", b"http", b"encoder"):
            self.assertNotIn(forbidden, lower_mp4)
        self.assertIn("autoplay", readme_text.lower())


if __name__ == "__main__":
    unittest.main()
