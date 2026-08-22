#!/usr/bin/env python3
"""Verify local ModernChic gameplay behavior without retaining its payload."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "tests/fixtures/beatoraja_skin/reference_manifest.json"
DEFAULT_TRACE = ROOT / "tests/fixtures/beatoraja_skin/traces/scuro_property_frames_v1.json"
DEFAULT_REPORTER = ROOT / "cmake-build-debug/gameplay_skin_loading_benchmark_tests"
AUDITOR = ROOT / "scripts/audit_beatoraja_skin.py"
PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
GRAPH_FAMILIES = {
    "noteDistribution",
    "timingVisualizer",
    "bpmGraph",
    "hitErrorVisualizer",
}
CALLBACK_FAILURE_CODES = {
    "skin_lua_allocator_limit_exceeded",
    "skin_lua_callback_result_invalid",
    "skin_lua_callback_script_failed",
    "skin_lua_event_execution_failed",
    "skin_lua_execution_failed",
    "skin_lua_frame_budget_exceeded",
    "skin_lua_instruction_limit_exceeded",
    "skin_lua_wall_time_limit_exceeded",
}
MAX_ARCHIVE_ENTRIES = 65_536
MAX_UNCOMPRESSED_BYTES = 2 * 1024 * 1024 * 1024


class VerificationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise VerificationError(message)


def object_at(value: Any, label: str, keys: set[str]) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail(f"{label} must be an object")
    if set(value) != keys:
        fail(f"{label} must contain exactly {', '.join(sorted(keys))}")
    return value


def array_at(value: Any, label: str) -> list[Any]:
    if not isinstance(value, list):
        fail(f"{label} must be an array")
    return value


def integer_at(value: Any, label: str, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        fail(f"{label} must be an integer of at least {minimum}")
    return value


def boolean_at(value: Any, label: str) -> bool:
    if not isinstance(value, bool):
        fail(f"{label} must be a boolean")
    return value


def string_array_at(value: Any, label: str) -> list[str]:
    result = array_at(value, label)
    if any(not isinstance(item, str) or not item for item in result):
        fail(f"{label} must contain only nonempty strings")
    if result != sorted(set(result)):
        fail(f"{label} must be sorted and unique")
    return result


def expected_long_note_oracle() -> dict[str, Any]:
    return {
        "source": {
            "path": "src/bms/player/beatoraja/play/LaneRenderer.java",
            "symbol": "LaneRenderer.drawLongNote",
            "jarConstants": "lib/jbms-parser.jar",
            "rule": (
                "explicit LongNote type wins; TYPE_UNDEFINED selects by "
                "BMSModel lntype 0=LN, 1=CN, 2=HCN"
            ),
        },
        "undefinedByModelType": [
            {
                "modelLntype": 0,
                "asoResolvedLnMode": 1,
                "mode": "LN",
                "bodyActive": 2,
                "bodyInactive": 3,
                "end": None,
                "start": 1,
            },
            {
                "modelLntype": 1,
                "asoResolvedLnMode": 2,
                "mode": "CN",
                "bodyActive": 2,
                "bodyInactive": 3,
                "end": 0,
                "start": 1,
            },
            {
                "modelLntype": 2,
                "asoResolvedLnMode": 3,
                "mode": "HCN",
                "bodyActive": 6,
                "bodyInactive": 7,
                "bodyReactive": 8,
                "bodyDamaged": 9,
                "end": 4,
                "start": 5,
            },
        ],
        "explicitTypeOverridesModel": [
            {"noteType": 1, "mode": "LN"},
            {"noteType": 2, "mode": "CN"},
            {"noteType": 3, "mode": "HCN"},
        ],
    }


def validate_long_note_trace(trace: Any) -> None:
    if not isinstance(trace, dict):
        fail("SCURO trace must be an object")
    if trace.get("schemaVersion") != 1 or trace.get("beatorajaCommit") != PINNED_COMMIT:
        fail("SCURO trace does not identify the pinned schema and Beatoraja commit")
    if trace.get("longNoteSelectorOracle") != expected_long_note_oracle():
        fail("SCURO long-note selector trace differs from pinned LaneRenderer slots")


def validate_session_report(
    raw: Any, trace: Any, expected_entry_identity: str
) -> dict[str, Any]:
    validate_long_note_trace(trace)
    report = object_at(
        raw,
        "session report",
        {
            "schemaVersion",
            "entryIdentity",
            "sessionPublished",
            "graphFamilies",
            "resourcePreparation",
            "callbackBudget",
            "unsupportedDiagnostics",
            "unsupportedSubjects",
            "diagnosticCodes",
            "loading",
        },
    )
    if report["schemaVersion"] != 1:
        fail("session report schemaVersion must be 1")
    if report["entryIdentity"] != expected_entry_identity:
        fail("session report entry identity does not match the audited selection")
    if boolean_at(report["sessionPublished"], "sessionPublished") is not True:
        fail("ModernChic gameplay session did not publish")

    graphs = object_at(report["graphFamilies"], "graphFamilies", GRAPH_FAMILIES)
    for family in sorted(GRAPH_FAMILIES):
        facts = object_at(graphs[family], f"graphFamilies.{family}", {"declared", "commands"})
        if integer_at(facts["declared"], f"graphFamilies.{family}.declared") == 0:
            fail(f"ModernChic does not declare required graph family {family}")
        if integer_at(facts["commands"], f"graphFamilies.{family}.commands") == 0:
            fail(f"ModernChic did not emit required graph family {family}")

    resources = object_at(
        report["resourcePreparation"],
        "resourcePreparation",
        {
            "complete",
            "allReferencedResourcesPrepared",
            "imageDecodes",
            "fontDecodes",
            "movieDecodes",
            "audioDecodes",
            "textureUploads",
        },
    )
    if not boolean_at(resources["complete"], "resourcePreparation.complete"):
        fail("ModernChic resource preparation did not complete")
    if not boolean_at(
        resources["allReferencedResourcesPrepared"],
        "resourcePreparation.allReferencedResourcesPrepared",
    ):
        fail("ModernChic did not materialize every planned referenced resource")
    for key in ("imageDecodes", "fontDecodes", "movieDecodes", "audioDecodes"):
        integer_at(resources[key], f"resourcePreparation.{key}")
    if integer_at(resources["textureUploads"], "resourcePreparation.textureUploads") == 0:
        fail("ModernChic resource preparation uploaded no textures")

    callbacks = object_at(
        report["callbackBudget"],
        "callbackBudget",
        {
            "frameBudgetMicros",
            "framesEvaluated",
            "maximumFrameWallMicros",
            "violationDiagnostics",
        },
    )
    frame_budget = integer_at(callbacks["frameBudgetMicros"], "callbackBudget.frameBudgetMicros", 1)
    if frame_budget != 6_000:
        fail("callback report must retain the production 6,000-microsecond frame budget")
    if integer_at(callbacks["framesEvaluated"], "callbackBudget.framesEvaluated") != 3:
        fail("callback report must contain exactly three fixed frames")
    if integer_at(callbacks["maximumFrameWallMicros"], "callbackBudget.maximumFrameWallMicros") > frame_budget:
        fail("ModernChic callback wall time exceeded the production frame budget")
    if string_array_at(callbacks["violationDiagnostics"], "callbackBudget.violationDiagnostics"):
        fail("ModernChic emitted a callback budget violation")

    if string_array_at(report["unsupportedDiagnostics"], "unsupportedDiagnostics"):
        fail("ModernChic emitted an unsupported compatibility diagnostic")
    if string_array_at(report["unsupportedSubjects"], "unsupportedSubjects"):
        fail("ModernChic requested an unsupported gameplay selector")
    diagnostic_codes = string_array_at(report["diagnosticCodes"], "diagnosticCodes")
    if CALLBACK_FAILURE_CODES.intersection(diagnostic_codes):
        fail("ModernChic emitted a Lua callback failure")
    loading = object_at(
        report["loading"], "loading", {"complete", "totalMicros", "measuredMicros"}
    )
    if not boolean_at(loading["complete"], "loading.complete"):
        fail("ModernChic loading telemetry is incomplete")
    if integer_at(loading["totalMicros"], "loading.totalMicros", 1) == 0:
        fail("ModernChic loading telemetry has no measured duration")
    integer_at(loading["measuredMicros"], "loading.measuredMicros", 1)
    return report


def read_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise VerificationError(f"{label} is unreadable or invalid JSON") from error
    if not isinstance(value, dict):
        fail(f"{label} must be a JSON object")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_extract_archive(archive: Path, destination: Path) -> None:
    seen: set[str] = set()
    total = 0
    try:
        with zipfile.ZipFile(archive) as package:
            entries = package.infolist()
            if not entries or len(entries) > MAX_ARCHIVE_ENTRIES:
                fail("ModernChic archive has an invalid entry count")
            for info in entries:
                path = PurePosixPath(info.filename)
                if (
                    not info.filename
                    or "\x00" in info.filename
                    or path.is_absolute()
                    or any(part in {"", ".", ".."} for part in path.parts)
                ):
                    fail("ModernChic archive contains an unsafe path")
                collision = "/".join(path.parts).casefold()
                if collision in seen:
                    fail("ModernChic archive contains a colliding path")
                seen.add(collision)
                mode = info.external_attr >> 16
                if stat.S_ISLNK(mode):
                    fail("ModernChic archive contains a symbolic link")
                total += info.file_size
                if total > MAX_UNCOMPRESSED_BYTES:
                    fail("ModernChic archive exceeds the extraction byte limit")
                target = destination.joinpath(*path.parts)
                if info.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                remaining = info.file_size
                with package.open(info) as source, target.open("xb") as output:
                    while remaining:
                        chunk = source.read(min(1024 * 1024, remaining))
                        if not chunk:
                            fail("ModernChic archive entry ended early")
                        output.write(chunk)
                        remaining -= len(chunk)
                    if source.read(1):
                        fail("ModernChic archive entry exceeded its declared size")
    except (OSError, zipfile.BadZipFile) as error:
        raise VerificationError("ModernChic archive could not be extracted") from error


def run_checked(command: list[str], label: str, timeout: int = 900) -> str:
    try:
        completed = subprocess.run(
            command,
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise VerificationError(f"{label} could not complete") from error
    if completed.returncode != 0:
        tail = "\n".join(completed.stdout.splitlines()[-20:])
        fail(f"{label} failed\n{tail}")
    return completed.stdout


def json_from_output(output: str, label: str) -> dict[str, Any]:
    for line in reversed(output.splitlines()):
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            return value
    fail(f"{label} emitted no JSON object")


def verified_inputs(
    skin: Path, temporary: Path, manifest: dict[str, Any]
) -> tuple[Path, Path]:
    archive_name = manifest.get("archiveFilename")
    expected_size = manifest.get("archiveByteCount")
    expected_digest = manifest.get("archiveSha256")
    prefix = manifest.get("archivePackagePrefix")
    if not all(isinstance(value, str) and value for value in (archive_name, expected_digest, prefix)):
        fail("reference manifest lacks ModernChic archive identity")
    if isinstance(expected_size, bool) or not isinstance(expected_size, int):
        fail("reference manifest lacks ModernChic archive byte count")

    skin = skin.resolve(strict=True)
    if skin.is_file():
        archive = skin
        if archive.name != archive_name:
            fail("ModernChic archive filename differs from the audited package")
        extraction = temporary / "extracted"
        extraction.mkdir()
        safe_extract_archive(archive, extraction)
        root = extraction / prefix
    elif skin.is_dir():
        root = skin
        archive = skin.parent / archive_name
    else:
        fail("--skin must name the audited archive or extracted directory")
    if not archive.is_file() or archive.stat().st_size != expected_size:
        fail("ModernChic archive byte count differs from the audited package")
    if sha256_file(archive) != expected_digest:
        fail("ModernChic archive SHA-256 differs from the audited package")
    if not root.is_dir():
        fail("ModernChic extracted package root is missing")
    return archive, root


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skin", required=True, type=Path)
    parser.add_argument("--beatoraja-root", required=True, type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--trace", type=Path, default=DEFAULT_TRACE)
    parser.add_argument("--session-reporter", type=Path, default=DEFAULT_REPORTER)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        manifest = read_json(arguments.manifest, "reference manifest")
        trace = read_json(arguments.trace, "SCURO trace")
        validate_long_note_trace(trace)
        if manifest.get("beatorajaCommit") != PINNED_COMMIT:
            fail("reference manifest does not pin the required Beatoraja commit")
        entry_records = manifest.get("entries")
        if not isinstance(entry_records, list) or len(entry_records) != 1:
            fail("reference manifest must select exactly one ModernChic gameplay entry")
        entry = entry_records[0]
        if not isinstance(entry, dict):
            fail("reference manifest ModernChic entry is invalid")
        entry_path = entry.get("path")
        entry_identity = entry.get("identity")
        if not all(isinstance(value, str) and value for value in (entry_path, entry_identity)):
            fail("reference manifest ModernChic entry lacks path or identity")
        beatoraja_root = arguments.beatoraja_root.resolve(strict=True)
        observed_commit = run_checked(
            ["git", "-C", str(beatoraja_root), "rev-parse", "HEAD"],
            "Beatoraja pin check",
            timeout=30,
        ).strip()
        if observed_commit != PINNED_COMMIT:
            fail("Beatoraja checkout is not at the pinned commit")
        reporter = arguments.session_reporter.resolve(strict=True)
        if not reporter.is_file() or not os.access(reporter, os.X_OK):
            fail("build gameplay_skin_loading_benchmark_tests before verification")

        with tempfile.TemporaryDirectory(prefix="asobmashow-modernchic-") as raw:
            temporary = Path(raw)
            archive, skin_root = verified_inputs(arguments.skin, temporary, manifest)
            run_checked(
                [
                    sys.executable,
                    str(AUDITOR),
                    "--beatoraja-root",
                    str(beatoraja_root),
                    "--archive-path",
                    str(archive),
                    "--archive-package-prefix",
                    str(manifest["archivePackagePrefix"]),
                    "--skin-root",
                    str(skin_root),
                    "--verify",
                    str(arguments.manifest),
                ],
                "ModernChic archive/source audit",
            )
            output = run_checked(
                [
                    str(reporter),
                    "--acceptance-report",
                    "--skin",
                    str(skin_root),
                    "--entry",
                    str(entry_path),
                    "--entry-identity",
                    str(entry_identity),
                    "--format",
                    "lua",
                ],
                "ModernChic gameplay session report",
            )
            session = validate_session_report(
                json_from_output(output, "ModernChic gameplay session report"),
                trace,
                entry_identity,
            )
            report = {
                "schemaVersion": 1,
                "beatorajaCommit": PINNED_COMMIT,
                "archiveSha256": manifest["archiveSha256"],
                "payloadTreeSha256": manifest["archivePayloadTreeSha256"],
                "entryIdentity": entry_identity,
                "longNoteSelectorOracle": trace["longNoteSelectorOracle"],
                "session": session,
            }
            report_path = temporary / "modernchic-acceptance-report.json"
            report_path.write_text(
                json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(report_path.read_text(encoding="utf-8"), end="")
        return 0
    except (VerificationError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
