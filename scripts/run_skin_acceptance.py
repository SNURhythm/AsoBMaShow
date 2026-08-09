#!/usr/bin/env python3
"""Validate the clone-independent SCURO acceptance contract and external evidence.

The verifier intentionally reads only a bounded JSON metadata file from the
external evidence root.  Screenshots and every other third-party payload stay
outside the repository and are identified only by their SHA-256 metadata.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable


MANIFEST_SCHEMA_VERSION = 1
ACCEPTANCE_SCHEMA_VERSION = 2
EVIDENCE_SCHEMA_VERSION = 2
SHA256 = re.compile(r"^[0-9a-f]{64}$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
OPAQUE_ID = re.compile(r"^[a-z0-9][a-z0-9:-]{3,127}$")
ISO_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
HARDWARE_MODEL = re.compile(r"^iPad\d+,\d+$")
OS_VERSION = re.compile(r"^\d+\.\d+(?:\.\d+)?$")
PUBLIC_URL = re.compile(r"https?://", re.IGNORECASE)
WINDOWS_ABSOLUTE = re.compile(r"^(?:[A-Za-z]:[\\/]|\\\\)")
MAX_METADATA_BYTES = 1024 * 1024
METADATA_FILENAME = "acceptance-evidence.json"
ORDINARY_RUNTIME_IO_OPERATIONS = (
    "filesystemRead",
    "filesystemWrite",
    "filesystemDirectoryScan",
)
LEGACY_RENDER_IO_FIELDS = frozenset({
    "negativeScenarios",
    "passingGuardVectorSha256",
    "overlayDigestBefore",
    "overlayDigestAfter",
    "deniedCountersExpected",
    "expectedDeniedOperation",
    "expectedDiagnostic",
    "expectedAction",
    "performedCountersExpected",
    "overlayDigestBeforeCapture",
    "overlayDigestAfterCapture",
    "overlayDigestComparison",
    "overlayDigestPolling",
})


class AcceptanceError(ValueError):
    """A deliberately sanitized contract or evidence diagnostic."""


def error(message: str) -> None:
    raise AcceptanceError(message)


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def object_at(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        error(f"{path} must be an object")
    return value


def array_at(value: Any, path: str) -> list[Any]:
    if not isinstance(value, list):
        error(f"{path} must be an array")
    return value


def required(mapping: dict[str, Any], key: str, path: str) -> Any:
    if key not in mapping:
        error(f"{path}.{key} is required")
    return mapping[key]


def string_at(value: Any, path: str) -> str:
    if not isinstance(value, str) or not value:
        error(f"{path} must be a nonempty string")
    return value


def integer_at(value: Any, path: str, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        error(f"{path} must be an integer >= {minimum}")
    return value


def sha256_at(value: Any, path: str) -> str:
    value = string_at(value, path)
    if not SHA256.fullmatch(value):
        error(f"{path} must be a lowercase SHA-256")
    return value


def opaque_id_at(value: Any, path: str) -> str:
    value = string_at(value, path)
    if not OPAQUE_ID.fullmatch(value):
        error(f"{path} must be an opaque identifier")
    return value


def status_at(value: Any, path: str) -> str:
    value = string_at(value, path)
    if value not in {"pending", "pass", "fail"}:
        error(f"{path} must be pending, pass, or fail")
    return value


def is_placeholder(value: Any) -> bool:
    return value is None or (isinstance(value, str) and (
        value == "pending" or "required" in value.lower()
    ))


def contains_absolute_path(value: str) -> bool:
    return (
        Path(value).is_absolute()
        or bool(WINDOWS_ABSOLUTE.match(value))
        or value.lower().startswith("file:")
    )


def walk_metadata(value: Any, path: tuple[str, ...] = ()) -> Iterable[tuple[tuple[str, ...], Any]]:
    yield path, value
    if isinstance(value, dict):
        for key, child in value.items():
            if not isinstance(key, str):
                error("metadata object keys must be strings")
            yield from walk_metadata(child, (*path, key))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from walk_metadata(child, (*path, str(index)))


def allowed_url_path(path: tuple[str, ...]) -> bool:
    return len(path) == 2 and path[0] in {"officialSource", "usageTerms"} and path[1] == "url"


def validate_safe_metadata(value: Any, *, label: str, allow_contract_urls: bool = False) -> None:
    for path, leaf in walk_metadata(value):
        if not path:
            continue
        key = path[-1].lower()
        rendered = ".".join(path)
        if any(token in key for token in ("udid", "account", "devicename", "device_name", "serialnumber", "serial_number", "identifierforvendor")):
            error(f"{label}.{rendered} contains a prohibited personal identifier field")
        if isinstance(leaf, str):
            if contains_absolute_path(leaf):
                error(f"{label}.{rendered} contains an absolute path")
            if PUBLIC_URL.search(leaf) and not (allow_contract_urls and allowed_url_path(path)):
                error(f"{label}.{rendered} contains a public URL outside provenance")


def read_json(path: Path, label: str) -> dict[str, Any]:
    try:
        payload = path.read_text(encoding="utf-8")
        value = json.loads(payload)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AcceptanceError(f"{label} is not readable JSON") from exc
    return object_at(value, label)


def status_objects(value: Any, path: str = "acceptanceContract") -> Iterable[tuple[str, dict[str, Any]]]:
    if isinstance(value, dict):
        if "status" in value:
            yield path, value
        for key, child in value.items():
            yield from status_objects(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from status_objects(child, f"{path}[{index}]")


def validate_statused_value(item: dict[str, Any], path: str) -> None:
    status = status_at(required(item, "status", path), f"{path}.status")
    if status == "pass":
        for key, value in item.items():
            if key != "status" and is_placeholder(value):
                error(f"{path}.{key} cannot be pending when status is pass")


def validate_pair_records(records: Any, path: str, *, screenshot: bool = False) -> None:
    expected = {("16:9", mode) for mode in ("fit", "stretch", "custom")} | {
        ("4:3", mode) for mode in ("fit", "stretch", "custom")
    }
    actual: set[tuple[str, str]] = set()
    for index, raw in enumerate(array_at(records, path)):
        item = object_at(raw, f"{path}[{index}]")
        aspect = string_at(required(item, "aspect", f"{path}[{index}]"), f"{path}[{index}].aspect")
        mode = string_at(required(item, "mode", f"{path}[{index}]"), f"{path}[{index}].mode")
        pair = (aspect, mode)
        if pair not in expected or pair in actual:
            error(f"{path} must contain each required layout exactly once")
        actual.add(pair)
        validate_statused_value(item, f"{path}[{index}]")
        if item["status"] == "pass":
            opaque_id_at(item.get("evidenceReference"), f"{path}[{index}].evidenceReference")
        if screenshot:
            timestamps = array_at(required(item, "timestampsMicros", f"{path}[{index}]"), f"{path}[{index}].timestampsMicros")
            if item["status"] == "pass" and not timestamps:
                error(f"{path}[{index}].timestampsMicros is required for pass")
            for timestamp in timestamps:
                integer_at(timestamp, f"{path}[{index}].timestampsMicros[]", 1)
    if actual != expected:
        error(f"{path} must contain all six Fit/Stretch/Custom layout cases")


def validate_ordinary_runtime_operations(value: Any, path: str) -> list[str]:
    operations = array_at(value, path)
    if len(operations) != len(set(operations)):
        error(f"{path} must not repeat an operation")
    for index, operation in enumerate(operations):
        if operation not in ORDINARY_RUNTIME_IO_OPERATIONS:
            error(f"{path}[{index}] is not an ordinary selected-root file-I/O operation")
    canonical = [operation for operation in ORDINARY_RUNTIME_IO_OPERATIONS if operation in operations]
    if operations != canonical:
        error(f"{path} must use canonical operation order")
    return operations


def validate_ordinary_runtime_io(value: Any, path: str) -> dict[str, Any]:
    ordinary = exact_object(value, path, {
        "status", "configuredLoadOperations", "renderCallbackOperations", "evidenceReference",
    })
    status = status_at(ordinary["status"], f"{path}.status")
    configured = validate_ordinary_runtime_operations(
        ordinary["configuredLoadOperations"], f"{path}.configuredLoadOperations"
    )
    render = validate_ordinary_runtime_operations(
        ordinary["renderCallbackOperations"], f"{path}.renderCallbackOperations"
    )
    if status == "pass":
        opaque_id_at(ordinary["evidenceReference"], f"{path}.evidenceReference")
    elif configured or render or ordinary["evidenceReference"] is not None:
        error(f"{path} pending or failed observations must remain empty")
    return ordinary


def reject_legacy_render_io_fields(acceptance: dict[str, Any]) -> None:
    for path, _ in walk_metadata(acceptance):
        if path and path[-1] in LEGACY_RENDER_IO_FIELDS:
            error("acceptanceContract contains legacy schema-v1 field: " + ".".join(path))


def validate_contract_schema(contract: dict[str, Any]) -> dict[str, Any]:
    if contract.get("schemaVersion") != MANIFEST_SCHEMA_VERSION:
        error("contract.schemaVersion must be 1")
    if not COMMIT.fullmatch(string_at(contract.get("beatorajaCommit"), "contract.beatorajaCommit")):
        error("contract.beatorajaCommit must be a lowercase commit")
    validate_safe_metadata(contract, label="contract", allow_contract_urls=True)

    acceptance = object_at(required(contract, "acceptanceContract", "contract"), "contract.acceptanceContract")
    reject_legacy_render_io_fields(acceptance)
    required_keys = {
        "autoplayScripts", "completionCriteria", "configuredHz", "drawableSize",
        "externalDigests", "hardwareModel", "iPadOS", "layouts", "limits",
        "measurementBuild", "ordinaryRuntimeIo", "physicalEvidence", "protocol",
        "safeInsets", "schemaVersion",
        "screenshotTimestamps", "syntheticChartHashes", "timerEventTrace",
    }
    if set(acceptance) != required_keys:
        missing = sorted(required_keys - set(acceptance))
        unexpected = sorted(set(acceptance) - required_keys)
        if missing:
            error("acceptanceContract is missing schema-v2 fields: " + ", ".join(missing))
        error("acceptanceContract has unexpected schema-v2 fields: " + ", ".join(unexpected))
    if acceptance.get("schemaVersion") != ACCEPTANCE_SCHEMA_VERSION:
        error("acceptanceContract.schemaVersion must be 2")

    for path, item in status_objects(acceptance):
        validate_statused_value(item, path)

    protocol = object_at(required(acceptance, "protocol", "acceptanceContract"), "acceptanceContract.protocol")
    if protocol != {"measurementSeconds": 180, "repetitions": 3, "warmupSeconds": 30}:
        error("acceptanceContract.protocol must preserve the frozen 30/180/3 protocol")

    limits = object_at(required(acceptance, "limits", "acceptanceContract"), "acceptanceContract.limits")
    expected_limits = {
        "liveResourceGrowthAfterTenExits": 0,
        "missedPresentationPercent": 0.5,
        "p99SkinCpuFrameFraction": 0.9,
        "residentMemoryDriftMiB": 32,
    }
    if limits != expected_limits:
        error("acceptanceContract.limits differs from the frozen schema-v2 limits")

    external = object_at(required(acceptance, "externalDigests", "acceptanceContract"), "acceptanceContract.externalDigests")
    for key in ("archiveSha256", "entrySha256", "payloadTreeSha256", "selectedLuaClosureSha256"):
        sha256_at(required(external, key, "acceptanceContract.externalDigests"), f"acceptanceContract.externalDigests.{key}")
    for key in ("activatedRevisionSha256", "configurationSha256"):
        wrapper = object_at(required(external, key, "acceptanceContract.externalDigests"), f"acceptanceContract.externalDigests.{key}")
        validate_statused_value(wrapper, f"acceptanceContract.externalDigests.{key}")
        if wrapper["status"] == "pass":
            sha256_at(wrapper["value"], f"acceptanceContract.externalDigests.{key}.value")
    if external["archiveSha256"] != contract.get("archiveSha256") or external["payloadTreeSha256"] != contract.get("archivePayloadTreeSha256"):
        error("acceptanceContract.externalDigests must match the frozen archive and payload tree")

    for name in ("hardwareModel", "iPadOS", "configuredHz", "drawableSize", "safeInsets", "measurementBuild", "physicalEvidence", "timerEventTrace"):
        object_at(required(acceptance, name, "acceptanceContract"), f"acceptanceContract.{name}")
    validate_ordinary_runtime_io(
        acceptance["ordinaryRuntimeIo"], "acceptanceContract.ordinaryRuntimeIo"
    )

    physical = exact_object(acceptance["physicalEvidence"], "acceptanceContract.physicalEvidence", {
        "accessControlledLocalEvidenceId", "deletionProcedure", "metadataFile",
        "recordId", "redactionStatus", "retentionUntil", "status",
    })
    if physical.get("metadataFile") != METADATA_FILENAME:
        error("acceptanceContract.physicalEvidence.metadataFile must be acceptance-evidence.json")
    if physical["status"] == "pass":
        for key in ("recordId", "accessControlledLocalEvidenceId", "redactionStatus", "retentionUntil", "deletionProcedure"):
            string_at(physical.get(key), f"acceptanceContract.physicalEvidence.{key}")
        opaque_id_at(physical["recordId"], "acceptanceContract.physicalEvidence.recordId")
        opaque_id_at(physical["accessControlledLocalEvidenceId"], "acceptanceContract.physicalEvidence.accessControlledLocalEvidenceId")
        opaque_id_at(physical["deletionProcedure"], "acceptanceContract.physicalEvidence.deletionProcedure")
        if physical["redactionStatus"] != "complete":
            error("acceptanceContract.physicalEvidence.redactionStatus must be complete")
        if not ISO_DATE.fullmatch(physical["retentionUntil"]):
            error("acceptanceContract.physicalEvidence.retentionUntil must be an ISO date")

    validate_pair_records(acceptance["layouts"], "acceptanceContract.layouts")
    validate_pair_records(acceptance["screenshotTimestamps"], "acceptanceContract.screenshotTimestamps", screenshot=True)
    for name in ("layouts", "screenshotTimestamps"):
        references = [
            item["evidenceReference"]
            for item in acceptance[name]
            if item["status"] == "pass"
        ]
        if len(references) != len(set(references)):
            error(f"acceptanceContract.{name} pass evidence references must be unique")
    layout_references = {
        (item["aspect"], item["mode"]): item
        for item in acceptance["layouts"]
    }
    for screenshot in acceptance["screenshotTimestamps"]:
        layout = layout_references[(screenshot["aspect"], screenshot["mode"])]
        if layout["status"] == "pass" and screenshot["status"] == "pass" and layout["evidenceReference"] != screenshot["evidenceReference"]:
            error("acceptanceContract.layouts evidence references must match screenshot records")

    for collection_name, hash_name in (("syntheticChartHashes", "sha256"), ("autoplayScripts", "scriptSha256")):
        collection = array_at(acceptance[collection_name], f"acceptanceContract.{collection_name}")
        if len(collection) != 4:
            error(f"acceptanceContract.{collection_name} must retain four scenarios")
        seen: set[str] = set()
        for index, raw in enumerate(collection):
            item = object_at(raw, f"acceptanceContract.{collection_name}[{index}]")
            scenario = string_at(required(item, "scenario", f"acceptanceContract.{collection_name}[{index}]"), f"acceptanceContract.{collection_name}[{index}].scenario")
            if scenario in seen:
                error(f"acceptanceContract.{collection_name} scenario IDs must be unique")
            seen.add(scenario)
            validate_statused_value(item, f"acceptanceContract.{collection_name}[{index}]")
            if item["status"] == "pass":
                sha256_at(item[hash_name], f"acceptanceContract.{collection_name}[{index}].{hash_name}")

    criteria = array_at(acceptance["completionCriteria"], "acceptanceContract.completionCriteria")
    if not criteria:
        error("acceptanceContract.completionCriteria cannot be empty")
    criterion_ids: set[str] = set()
    for index, raw in enumerate(criteria):
        item = object_at(raw, f"acceptanceContract.completionCriteria[{index}]")
        criterion = string_at(required(item, "id", f"acceptanceContract.completionCriteria[{index}]"), f"acceptanceContract.completionCriteria[{index}].id")
        if criterion in criterion_ids:
            error("acceptanceContract.completionCriteria IDs must be unique")
        criterion_ids.add(criterion)
        validate_statused_value(item, f"acceptanceContract.completionCriteria[{index}]")
        if item["status"] == "pass":
            opaque_id_at(item["evidenceReference"], f"acceptanceContract.completionCriteria[{index}].evidenceReference")

    for name in ("hardwareModel", "iPadOS", "configuredHz", "drawableSize", "safeInsets", "measurementBuild"):
        wrapper = acceptance[name]
        if wrapper["status"] == "pass" and is_placeholder(wrapper.get("value")):
            error(f"acceptanceContract.{name}.value is required for pass")
    if acceptance["configuredHz"]["status"] == "pass":
        integer_at(acceptance["configuredHz"]["value"], "acceptanceContract.configuredHz.value", 1)
        if acceptance["configuredHz"]["value"] > 240:
            error("acceptanceContract.configuredHz.value must not exceed 240")
    if acceptance["hardwareModel"]["status"] == "pass" and not HARDWARE_MODEL.fullmatch(acceptance["hardwareModel"]["value"]):
        error("acceptanceContract.hardwareModel.value must be a non-unique iPad model identifier")
    if acceptance["iPadOS"]["status"] == "pass" and not OS_VERSION.fullmatch(acceptance["iPadOS"]["value"]):
        error("acceptanceContract.iPadOS.value must be an exact iPadOS version")
    if acceptance["drawableSize"]["status"] == "pass":
        drawable = object_at(acceptance["drawableSize"]["value"], "acceptanceContract.drawableSize.value")
        if set(drawable) != {"width", "height"}:
            error("acceptanceContract.drawableSize.value must contain width and height")
        integer_at(drawable["width"], "acceptanceContract.drawableSize.value.width", 1)
        integer_at(drawable["height"], "acceptanceContract.drawableSize.value.height", 1)
    if acceptance["safeInsets"]["status"] == "pass":
        insets = object_at(acceptance["safeInsets"]["value"], "acceptanceContract.safeInsets.value")
        if set(insets) != {"top", "left", "bottom", "right"}:
            error("acceptanceContract.safeInsets.value must contain four inset values")
        for key, value in insets.items():
            integer_at(value, f"acceptanceContract.safeInsets.value.{key}", 0)
    if acceptance["measurementBuild"]["status"] == "pass":
        build = object_at(acceptance["measurementBuild"]["value"], "acceptanceContract.measurementBuild.value")
        if not COMMIT.fullmatch(string_at(build.get("commit"), "acceptanceContract.measurementBuild.value.commit")):
            error("acceptanceContract.measurementBuild.value.commit must be a lowercase commit")
        string_at(build.get("configuration"), "acceptanceContract.measurementBuild.value.configuration")
        if build.get("sourceClean") is not True:
            error("acceptanceContract.measurementBuild.value.sourceClean must be true")

    trace = acceptance["timerEventTrace"]
    if trace["status"] == "pass":
        selected = array_at(trace.get("selectedIds"), "acceptanceContract.timerEventTrace.selectedIds")
        observed = array_at(trace.get("observedOrder"), "acceptanceContract.timerEventTrace.observedOrder")
        if not selected or not observed:
            error("acceptanceContract.timerEventTrace must retain selected IDs and observed order for pass")
        for value in (*selected, *observed):
            integer_at(value, "acceptanceContract.timerEventTrace ID", 0)
        opaque_id_at(trace.get("evidenceReference"), "acceptanceContract.timerEventTrace.evidenceReference")

    payload_digests: set[str] = set()
    for index, raw in enumerate(array_at(contract.get("externalPayloadDigests"), "contract.externalPayloadDigests")):
        item = object_at(raw, f"contract.externalPayloadDigests[{index}]")
        payload_digests.add(sha256_at(required(item, "sha256", f"contract.externalPayloadDigests[{index}]"), f"contract.externalPayloadDigests[{index}].sha256"))
    if not payload_digests:
        error("contract.externalPayloadDigests cannot be empty")

    expected_fixture_paths = (
        "charts/acceptance_7k.bms",
        "charts/acceptance_bga_base.png",
        "charts/acceptance_bga_layer.png",
        "charts/acceptance_bga_miss.png",
        "charts/acceptance_bga_video.mp4",
    )
    fixtures = array_at(
        contract.get("redistributableSyntheticFixtureDigests"),
        "contract.redistributableSyntheticFixtureDigests",
    )
    if len(fixtures) != len(expected_fixture_paths):
        error("contract.redistributableSyntheticFixtureDigests must contain the exact five fixtures")
    fixture_digests: dict[str, str] = {}
    for index, raw in enumerate(fixtures):
        path = f"contract.redistributableSyntheticFixtureDigests[{index}]"
        item = exact_object(object_at(raw, path), path, {"path", "sha256"})
        relative_path = string_at(item["path"], f"{path}.path")
        digest = sha256_at(item["sha256"], f"{path}.sha256")
        if relative_path in fixture_digests:
            error("contract.redistributableSyntheticFixtureDigests paths must be unique")
        fixture_digests[relative_path] = digest
    if tuple(fixture_digests) != expected_fixture_paths:
        error("contract.redistributableSyntheticFixtureDigests must use the exact fixture paths and order")
    fixture_root = repository_root() / "tests/fixtures/beatoraja_skin"
    for relative_path, recorded_digest in fixture_digests.items():
        fixture = fixture_root / relative_path
        try:
            payload = fixture.read_bytes()
        except OSError as exc:
            raise AcceptanceError("redistributable synthetic fixture is unreadable") from exc
        if hashlib.sha256(payload).hexdigest() != recorded_digest:
            error("contract.redistributableSyntheticFixtureDigests does not match checked-in fixture bytes")
    return acceptance


def tracked_payload_digest_check(root: Path, payload_digests: set[str]) -> None:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except OSError as exc:
        raise AcceptanceError("unable to enumerate tracked files") from exc
    if result.returncode != 0:
        error("unable to enumerate tracked files")
    for encoded in result.stdout.split(b"\0"):
        if not encoded:
            continue
        relative = Path(os.fsdecode(encoded))
        candidate = root / relative
        try:
            mode = candidate.lstat().st_mode
        except OSError as exc:
            raise AcceptanceError("unable to inspect a tracked file") from exc
        if not stat.S_ISREG(mode):
            continue
        digest = hashlib.sha256()
        try:
            with candidate.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
        except OSError as exc:
            raise AcceptanceError("unable to hash a tracked file") from exc
        if digest.hexdigest() in payload_digests:
            error("a tracked file matches an audited SCURO payload digest")


def require_all_pass(acceptance: dict[str, Any]) -> None:
    for path, item in status_objects(acceptance):
        if status_at(item["status"], f"{path}.status") != "pass":
            error(f"{path}.status is pending or failed; physical acceptance requires pass")


def external_root(path: Path, root: Path) -> Path:
    try:
        resolved = path.resolve(strict=True)
    except OSError as exc:
        raise AcceptanceError("evidence root does not exist") from exc
    if not resolved.is_dir():
        error("evidence root is not a directory")
    try:
        resolved.relative_to(root)
    except ValueError:
        return resolved
    error("evidence root is inside repository")


def read_evidence(root: Path, filename: str) -> dict[str, Any]:
    candidate = root / filename
    try:
        candidate_mode = candidate.lstat().st_mode
        metadata = candidate.resolve(strict=True)
        mode = metadata.lstat().st_mode
    except OSError as exc:
        raise AcceptanceError("external evidence metadata is missing") from exc
    if stat.S_ISLNK(candidate_mode) or metadata.parent != root or not stat.S_ISREG(mode):
        error("external evidence metadata must be a direct regular file")
    try:
        if metadata.stat().st_size > MAX_METADATA_BYTES:
            error("external evidence metadata exceeds the bounded size")
    except OSError as exc:
        raise AcceptanceError("external evidence metadata is unreadable") from exc
    return read_json(metadata, "external evidence metadata")


def exact_object(value: Any, path: str, keys: set[str]) -> dict[str, Any]:
    item = object_at(value, path)
    if set(item) != keys:
        missing = sorted(keys - set(item))
        unexpected = sorted(set(item) - keys)
        if missing:
            error(f"{path} is missing required fields: {', '.join(missing)}")
        error(f"{path} has unexpected fields: {', '.join(unexpected)}")
    return item


def finite_number_at(value: Any, path: str, minimum: float, maximum: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        error(f"{path} must be a finite number")
    rendered = float(value)
    if not math.isfinite(rendered) or not minimum <= rendered <= maximum:
        error(f"{path} is outside its permitted range")
    return rendered


def signed_integer_at(value: Any, path: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        error(f"{path} must be an integer")
    return value


def exact_counter_record(value: Any, path: str, keys: set[str]) -> dict[str, int]:
    counters = exact_object(value, path, keys)
    return {
        key: integer_at(counters[key], f"{path}.{key}", 0)
        for key in keys
    }


def validate_performance_runs(acceptance: dict[str, Any], evidence: dict[str, Any]) -> None:
    chart_hashes = {
        item["scenario"]: item["sha256"]
        for item in acceptance["syntheticChartHashes"]
    }
    autoplay_hashes = {
        item["scenario"]: item["scriptSha256"]
        for item in acceptance["autoplayScripts"]
    }
    layout_pairs = {
        (item["aspect"], item["mode"])
        for item in acceptance["layouts"]
    }
    expected_runs = {
        (scenario, aspect, mode, repetition)
        for scenario in chart_hashes
        for aspect, mode in layout_pairs
        for repetition in range(1, 4)
    }
    external = acceptance["externalDigests"]
    expected_refresh = acceptance["configuredHz"]["value"]
    expected_fields = {
        "scenario", "aspect", "mode", "repetition", "chartSha256",
        "autoplayScriptSha256", "activatedRevisionSha256", "configurationSha256",
        "warmupStartMicros", "recordingStartMicros", "recordingEndMicros",
        "configuredRefreshHz", "p99SkinCpuMicros", "missedPresentationPercent",
        "residentDriftBytes", "telemetry",
    }
    telemetry_fields = {
        "receivedSampleCount", "retainedSampleCount", "overflowSampleCount",
        "incompleteSampleCount", "mismatchedSampleCount",
    }
    observed: set[tuple[str, str, str, int]] = set()
    for index, raw in enumerate(array_at(evidence["performanceRuns"], "external evidence metadata.performanceRuns")):
        path = f"external evidence metadata.performanceRuns[{index}]"
        run = exact_object(raw, path, expected_fields)
        scenario = string_at(run["scenario"], f"{path}.scenario")
        pair = (string_at(run["aspect"], f"{path}.aspect"), string_at(run["mode"], f"{path}.mode"))
        repetition = integer_at(run["repetition"], f"{path}.repetition", 1)
        identity = (scenario, *pair, repetition)
        if identity not in expected_runs or identity in observed:
            error("external evidence metadata.performanceRuns must contain each required scenario/layout repetition exactly once")
        observed.add(identity)
        if run["chartSha256"] != chart_hashes[scenario] or run["autoplayScriptSha256"] != autoplay_hashes[scenario]:
            error("external evidence metadata performance chart or autoplay digest does not match the contract")
        for key in ("chartSha256", "autoplayScriptSha256", "activatedRevisionSha256", "configurationSha256"):
            sha256_at(run[key], f"{path}.{key}")
        if run["activatedRevisionSha256"] != external["activatedRevisionSha256"]["value"] or run["configurationSha256"] != external["configurationSha256"]["value"]:
            error("external evidence metadata performance activation digest does not match the contract")
        warmup_start = integer_at(run["warmupStartMicros"], f"{path}.warmupStartMicros", 0)
        recording_start = integer_at(run["recordingStartMicros"], f"{path}.recordingStartMicros", 0)
        recording_end = integer_at(run["recordingEndMicros"], f"{path}.recordingEndMicros", 0)
        if recording_start - warmup_start != 30_000_000 or recording_end - recording_start != 180_000_000:
            error("external evidence metadata performance run must preserve the trusted 30-second warm-up and 180-second measurement")
        if run["configuredRefreshHz"] != expected_refresh:
            error("external evidence metadata performance refresh rate does not match the contract")
        p99 = integer_at(run["p99SkinCpuMicros"], f"{path}.p99SkinCpuMicros", 0)
        if p99 * expected_refresh * 10 > 9_000_000:
            error("external evidence metadata performance p99 exceeds 90 percent of the refresh interval")
        finite_number_at(run["missedPresentationPercent"], f"{path}.missedPresentationPercent", 0.0, acceptance["limits"]["missedPresentationPercent"])
        if abs(signed_integer_at(run["residentDriftBytes"], f"{path}.residentDriftBytes")) > acceptance["limits"]["residentMemoryDriftMiB"] * 1024 * 1024:
            error("external evidence metadata performance resident-memory drift exceeds the contract limit")
        telemetry = exact_counter_record(run["telemetry"], f"{path}.telemetry", telemetry_fields)
        if telemetry["receivedSampleCount"] == 0 or telemetry["retainedSampleCount"] != telemetry["receivedSampleCount"] or any(telemetry[key] != 0 for key in ("overflowSampleCount", "incompleteSampleCount", "mismatchedSampleCount")):
            error("external evidence metadata performance telemetry is incomplete, mismatched, or overflowed")
    if observed != expected_runs:
        error("external evidence metadata performance runs are missing a required scenario/layout repetition")


def validate_resource_lifecycle(evidence: dict[str, Any]) -> None:
    lifecycle = exact_object(evidence["resourceLifecycle"], "external evidence metadata.resourceLifecycle", {"baseline", "postDestruction"})
    counter_fields = {"liveTextures", "liveResources", "residentBytes"}
    baseline = exact_counter_record(lifecycle["baseline"], "external evidence metadata.resourceLifecycle.baseline", counter_fields)
    observed: set[int] = set()
    for index, raw in enumerate(array_at(lifecycle["postDestruction"], "external evidence metadata.resourceLifecycle.postDestruction")):
        path = f"external evidence metadata.resourceLifecycle.postDestruction[{index}]"
        sample = exact_object(raw, path, {"cycle", *counter_fields})
        cycle = integer_at(sample["cycle"], f"{path}.cycle", 1)
        if cycle not in range(1, 11) or cycle in observed:
            error("external evidence metadata resource lifecycle must contain exactly ten unique teardown cycles")
        observed.add(cycle)
        counters = exact_counter_record({key: sample[key] for key in counter_fields}, path, counter_fields)
        if counters["liveTextures"] != baseline["liveTextures"] or counters["liveResources"] != baseline["liveResources"]:
            error("external evidence metadata resource lifecycle did not return live resources to baseline")
    if observed != set(range(1, 11)):
        error("external evidence metadata resource lifecycle must contain all ten post-destruction samples")


def validate_ordinary_runtime_io_evidence(
    acceptance: dict[str, Any], evidence: dict[str, Any]
) -> None:
    expected = acceptance["ordinaryRuntimeIo"]
    observed = exact_object(
        evidence["ordinaryRuntimeIo"],
        "external evidence metadata.ordinaryRuntimeIo",
        {"configuredLoadOperations", "renderCallbackOperations", "evidenceReference"},
    )
    configured = validate_ordinary_runtime_operations(
        observed["configuredLoadOperations"],
        "external evidence metadata.ordinaryRuntimeIo.configuredLoadOperations",
    )
    render = validate_ordinary_runtime_operations(
        observed["renderCallbackOperations"],
        "external evidence metadata.ordinaryRuntimeIo.renderCallbackOperations",
    )
    evidence_reference = opaque_id_at(
        observed["evidenceReference"],
        "external evidence metadata.ordinaryRuntimeIo.evidenceReference",
    )
    if (
        configured != expected["configuredLoadOperations"]
        or render != expected["renderCallbackOperations"]
        or evidence_reference != expected["evidenceReference"]
    ):
        error("external evidence metadata ordinary runtime I/O observations do not match the contract")


def validate_evidence(acceptance: dict[str, Any], evidence: dict[str, Any], expected_commit: str) -> None:
    validate_safe_metadata(evidence, label="externalEvidence")
    evidence = exact_object(evidence, "external evidence metadata", {
        "schemaVersion", "recordId", "accessControlledLocalEvidenceId", "redactionStatus",
        "retentionUntil", "deletionProcedure", "completionEvidence", "screenshots",
        "performanceRuns", "resourceLifecycle", "ordinaryRuntimeIo",
    })
    if evidence.get("schemaVersion") != EVIDENCE_SCHEMA_VERSION:
        error("external evidence metadata.schemaVersion must be 2")
    physical = acceptance["physicalEvidence"]
    for key in ("recordId", "accessControlledLocalEvidenceId", "redactionStatus", "retentionUntil", "deletionProcedure"):
        if evidence.get(key) != physical.get(key):
            error(f"external evidence metadata.{key} does not match the contract")
    if evidence["redactionStatus"] != "complete" or not ISO_DATE.fullmatch(evidence["retentionUntil"]):
        error("external evidence metadata lacks completed redaction or retention metadata")
    opaque_id_at(evidence["deletionProcedure"], "external evidence metadata.deletionProcedure")

    build = object_at(acceptance["measurementBuild"]["value"], "acceptanceContract.measurementBuild.value")
    if build["commit"] != expected_commit:
        error("measurement build commit does not match expected app commit")

    evidence_ids = set()
    for index, raw in enumerate(array_at(evidence.get("completionEvidence"), "external evidence metadata.completionEvidence")):
        item = object_at(raw, f"external evidence metadata.completionEvidence[{index}]")
        if set(item) != {"id"}:
            error("external evidence metadata completion records must contain only opaque IDs")
        evidence_id = opaque_id_at(item["id"], f"external evidence metadata.completionEvidence[{index}].id")
        if evidence_id in evidence_ids:
            error("external evidence metadata completion records must not be duplicated")
        evidence_ids.add(evidence_id)
    expected_ids = {item["evidenceReference"] for item in acceptance["completionCriteria"]}
    if evidence_ids != expected_ids:
        error("external evidence metadata completion records do not match the contract")

    expected_screenshots = {
        (item["aspect"], item["mode"]): item
        for item in acceptance["screenshotTimestamps"]
    }
    observed: set[tuple[str, str]] = set()
    for index, raw in enumerate(array_at(evidence.get("screenshots"), "external evidence metadata.screenshots")):
        item = object_at(raw, f"external evidence metadata.screenshots[{index}]")
        required_fields = {"evidenceId", "aspect", "mode", "sha256", "width", "height", "timestampMicros"}
        if set(item) != required_fields:
            error("external evidence metadata screenshot records must contain metadata only")
        pair = (string_at(item["aspect"], f"external evidence metadata.screenshots[{index}].aspect"), string_at(item["mode"], f"external evidence metadata.screenshots[{index}].mode"))
        expected = expected_screenshots.get(pair)
        if expected is None or pair in observed:
            error("external evidence metadata screenshot layouts do not match the contract")
        observed.add(pair)
        if item["evidenceId"] != expected["evidenceReference"]:
            error("external evidence metadata screenshot evidence ID does not match the contract")
        opaque_id_at(item["evidenceId"], f"external evidence metadata.screenshots[{index}].evidenceId")
        sha256_at(item["sha256"], f"external evidence metadata.screenshots[{index}].sha256")
        integer_at(item["width"], f"external evidence metadata.screenshots[{index}].width", 1)
        integer_at(item["height"], f"external evidence metadata.screenshots[{index}].height", 1)
        timestamp = integer_at(item["timestampMicros"], f"external evidence metadata.screenshots[{index}].timestampMicros", 1)
        if timestamp not in expected["timestampsMicros"]:
            error("external evidence metadata screenshot timestamp does not match the contract")
    if observed != set(expected_screenshots):
        error("external evidence metadata must contain one screenshot metadata record per layout")
    validate_performance_runs(acceptance, evidence)
    validate_resource_lifecycle(evidence)
    validate_ordinary_runtime_io_evidence(acceptance, evidence)


def validate(contract_path: Path) -> None:
    contract = read_json(contract_path, "contract")
    validate_contract_schema(contract)
    payload_digests = {
        item["sha256"] for item in array_at(contract["externalPayloadDigests"], "contract.externalPayloadDigests")
    }
    tracked_payload_digest_check(repository_root(), payload_digests)


def verify(contract_path: Path, evidence_root_path: Path, expected_commit: str) -> None:
    if not COMMIT.fullmatch(expected_commit):
        error("expected app commit must be a lowercase 40-character commit")
    evidence_root = external_root(evidence_root_path, repository_root())
    contract = read_json(contract_path, "contract")
    acceptance = validate_contract_schema(contract)
    payload_digests = {
        item["sha256"] for item in array_at(contract["externalPayloadDigests"], "contract.externalPayloadDigests")
    }
    tracked_payload_digest_check(repository_root(), payload_digests)
    require_all_pass(acceptance)
    evidence = read_evidence(evidence_root, acceptance["physicalEvidence"]["metadataFile"])
    validate_evidence(acceptance, evidence, expected_commit)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    validate_parser = subcommands.add_parser("validate", help="validate the committed schema-v2 acceptance contract")
    validate_parser.add_argument("--contract", required=True, type=Path)
    verify_parser = subcommands.add_parser("verify", help="validate completed external physical evidence")
    verify_parser.add_argument("--contract", required=True, type=Path)
    verify_parser.add_argument("--evidence-root", required=True, type=Path)
    verify_parser.add_argument("--expected-app-commit", required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.command == "validate":
            validate(arguments.contract)
            print("schema-v2 acceptance contract valid")
        else:
            verify(arguments.contract, arguments.evidence_root, arguments.expected_app_commit)
            print("physical SCURO acceptance evidence verified")
    except AcceptanceError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
