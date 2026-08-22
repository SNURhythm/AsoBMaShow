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
DEFAULT_TRACE = ROOT / "tests/fixtures/beatoraja_skin/traces/gameplay_objects_pinned_v1.json"
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


def validate_long_note_trace(trace: Any) -> None:
    if not isinstance(trace, dict):
        fail("gameplay oracle trace must be an object")
    if trace.get("schemaVersion") != 1 or trace.get("referenceCommit") != PINNED_COMMIT:
        fail("gameplay oracle does not identify the pinned Beatoraja commit")
    oracle = trace.get("longNoteOracle")
    if not isinstance(oracle, dict) or oracle.get("generation") != (
        "pinned-LaneRenderer-lexical-slots+jvm-JBMS-constants"
    ):
        fail("long-note oracle was not generated from pinned source and constants")
    source = oracle.get("source")
    if not isinstance(source, dict) or source.get("path") != (
        "src/bms/player/beatoraja/play/LaneRenderer.java"
    ) or source.get("symbol") != "LaneRenderer.drawLongNote" or not isinstance(
        source.get("sha256"), str
    ) or len(source["sha256"]) != 64:
        fail("long-note oracle lacks pinned LaneRenderer provenance")
    constants = oracle.get("constants")
    slots = oracle.get("drawSlots")
    if not isinstance(constants, dict) or not isinstance(slots, dict):
        fail("long-note oracle lacks generated constants or draw slots")
    modes = [constants.get("noteLN"), constants.get("noteCN"), constants.get("noteHCN")]
    if any(isinstance(mode, bool) or not isinstance(mode, int) for mode in modes) or \
       set(slots) != {str(mode) for mode in modes}:
        fail("long-note oracle mode/slot identity is inconsistent")
    expected_undefined = [
        {"modelLntype": constants.get("modelLN"),
         "selectedMode": constants.get("noteLN"), "mode": "LN"},
        {"modelLntype": constants.get("modelCN"),
         "selectedMode": constants.get("noteCN"), "mode": "CN"},
        {"modelLntype": constants.get("modelHCN"),
         "selectedMode": constants.get("noteHCN"), "mode": "HCN"},
    ]
    expected_explicit = [
        {"noteType": constants.get("noteLN"), "mode": "LN"},
        {"noteType": constants.get("noteCN"), "mode": "CN"},
        {"noteType": constants.get("noteHCN"), "mode": "HCN"},
    ]
    if oracle.get("undefinedByModelType") != expected_undefined or \
       oracle.get("explicitTypeOverridesModel") != expected_explicit:
        fail("long-note selector mapping differs from generated constants")


def validate_runtime_matrix(
    matrix: Any, entries: Any, trace: Any
) -> list[dict[str, Any]]:
    validate_long_note_trace(trace)
    if not isinstance(entries, list) or len(entries) != 4:
        fail("ModernChic manifest must declare four gameplay entries")
    expected_keys = {5, 7, 10, 14}
    if {entry.get("keys") for entry in entries if isinstance(entry, dict)} != expected_keys:
        fail("ModernChic gameplay entries must cover 5/7/10/14 keys")
    identities = {
        entry.get("identity"): entry.get("keys") for entry in entries
        if isinstance(entry, dict)
    }
    oracle = trace["longNoteOracle"]
    constants = oracle["constants"]
    modes = [constants["noteLN"], constants["noteCN"], constants["noteHCN"]]
    cells = array_at(matrix, "runtimeMatrix")
    expected_cells = {(identity, mode) for identity in identities for mode in modes}
    observed_cells: set[tuple[str, int]] = set()
    for index, raw in enumerate(cells):
        cell = object_at(raw, f"runtimeMatrix[{index}]", {
            "entryIdentity", "keys", "lnMode", "sessionPublished",
            "selectorIndex", "drawSlots", "referencedImageResourceIds",
            "preparedImageResourceIds", "referencedTextObjectIds",
            "preparedTextObjectIds", "unsupportedDiagnostics",
        })
        identity = cell["entryIdentity"]
        mode = cell["lnMode"]
        if identity not in identities or mode not in modes or \
           cell["keys"] != identities[identity]:
            fail("runtime matrix cell has an unknown entry or long-note mode")
        if (identity, mode) in observed_cells:
            fail("runtime matrix contains a duplicate entry/mode cell")
        observed_cells.add((identity, mode))
        if boolean_at(cell["sessionPublished"], "sessionPublished") is not True:
            fail("ModernChic runtime matrix session did not publish")
        if integer_at(cell["selectorIndex"], "selectorIndex") != modes.index(mode):
            fail("ModernChic long-note selector differs from generated constants")
        if cell["drawSlots"] != oracle["drawSlots"][str(mode)]:
            fail("ModernChic long-note draw slots differ from pinned LaneRenderer")
        for key in (
            "referencedImageResourceIds", "preparedImageResourceIds",
            "referencedTextObjectIds", "preparedTextObjectIds",
        ):
            values = array_at(cell[key], key)
            if values != sorted(set(values)) or any(
                isinstance(value, bool) or not isinstance(value, int) or value <= 0
                for value in values
            ):
                fail(f"{key} must contain sorted unique positive IDs")
        if cell["referencedImageResourceIds"] != cell["preparedImageResourceIds"]:
            fail("ModernChic referenced images differ from independently prepared IDs")
        if cell["referencedTextObjectIds"] != cell["preparedTextObjectIds"]:
            fail("ModernChic referenced text objects differ from prepared atlas owners")
        if string_array_at(cell["unsupportedDiagnostics"], "unsupportedDiagnostics"):
            fail("ModernChic matrix cell emitted an unsupported diagnostic")
    if observed_cells != expected_cells:
        fail("ModernChic runtime matrix is incomplete")
    return cells


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


def json_array_from_output(output: str, label: str) -> list[Any]:
    for line in reversed(output.splitlines()):
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, list):
            return value
    fail(f"{label} emitted no JSON array")


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
        if not isinstance(entry_records, list) or len(entry_records) != 4 or any(
            not isinstance(entry, dict) for entry in entry_records
        ):
            fail("reference manifest must select four ModernChic gameplay entries")
        selected = [entry for entry in entry_records if entry.get("keys") == 7]
        if len(selected) != 1:
            fail("reference manifest must select exactly one 7-key gameplay entry")
        entry = selected[0]
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
            matrix_output = run_checked(
                [
                    str(reporter),
                    "--acceptance-matrix",
                    "--skin",
                    str(skin_root),
                    "--format",
                    "lua",
                ],
                "ModernChic gameplay runtime matrix",
            )
            entries_by_path = {
                record["path"]: record["identity"] for record in entry_records
            }
            raw_matrix = json_array_from_output(
                matrix_output, "ModernChic gameplay runtime matrix"
            )
            runtime_matrix = []
            for index, cell in enumerate(raw_matrix):
                if not isinstance(cell, dict) or "entryPath" not in cell or (
                    "entryIdentity" in cell
                ):
                    fail(f"runtime matrix cell {index} is invalid")
                entry_path_value = cell.get("entryPath")
                if entry_path_value not in entries_by_path:
                    fail(f"runtime matrix cell {index} names an unaudited entry")
                sanitized = dict(cell)
                del sanitized["entryPath"]
                sanitized["entryIdentity"] = entries_by_path[entry_path_value]
                runtime_matrix.append(sanitized)
            validate_runtime_matrix(runtime_matrix, entry_records, trace)
            report = {
                "schemaVersion": 1,
                "beatorajaCommit": PINNED_COMMIT,
                "archiveSha256": manifest["archiveSha256"],
                "payloadTreeSha256": manifest["archivePayloadTreeSha256"],
                "entryIdentities": sorted(
                    record["identity"] for record in entry_records
                ),
                "longNoteOracle": trace["longNoteOracle"],
                "runtimeMatrix": runtime_matrix,
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
