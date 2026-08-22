#!/usr/bin/env python3
"""Validate the committed Beatoraja gameplay skin parity contract."""

import json
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
SCHEMA_VERSION = 1
SOURCE_SURFACE_PATH = (
    ROOT / "docs/skin-compat/beatoraja-gameplay-source-surface-v1.json"
)
LEDGER_PATH = ROOT / "docs/skin-compat/beatoraja-gameplay-feature-ledger-v1.json"
EXTRACTOR_PATH = ROOT / "scripts/extract_beatoraja_gameplay_skin_surface.py"
VALID_STATUS = {"implemented", "missing", "source-defined-noop"}
FORBIDDEN_CLASSIFICATIONS = {"unclassified", "tbd", "todo"}
TEST_RUNNERS = {
    "tests/json_gameplay_skin_decoder_tests.cpp": "json_gameplay_skin_decoder_tests",
    "tests/beatoraja_skin_model_tests.cpp": "beatoraja_skin_model_tests",
    "tests/lr2_gameplay_skin_decoder_tests.cpp": "lr2_gameplay_skin_decoder_tests",
    "tests/lr2_skin_csv_parser_tests.cpp": "lr2_skin_csv_parser_tests",
    "tests/lua_skin_host_modules_tests.cpp": "lua_skin_host_modules_tests",
    "tests/play_skin_session_tests.cpp": "play_skin_session_tests",
    "tests/input_device_registry_tests.cpp": "input_device_registry_tests",
    "tests/gameplay_skin_session_factory_tests.cpp": "gameplay_skin_session_factory_tests",
    "tests/lua_skin_http_transport_tests.cpp": "lua_skin_http_transport_tests",
    "tests/lua_skin_file_system_tests.cpp": "lua_skin_file_system_tests",
    "tests/lua_skin_text_graph_live_integration_tests.cpp": "lua_skin_text_graph_live_integration_tests",
    "tests/audio_mix_tests.cpp": "audio_mix_tests",
    "tests/audio_wrapper_lifecycle_tests.cpp": "audio_wrapper_lifecycle_tests",
}


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def assert_unique_ids(features: list[dict], label: str) -> set[str]:
    ids = [feature["id"] for feature in features]
    assert len(ids) == len(set(ids)), f"{label} contains duplicate IDs"
    return set(ids)


def assert_sorted_ids(features: list[dict], label: str) -> None:
    ids = [feature["id"] for feature in features]
    assert ids == sorted(ids), f"{label} IDs must remain sorted"


def feature_by_id(features: list[dict]) -> dict[str, dict]:
    return {feature["id"]: feature for feature in features}


def validate_executed_coverage(
    expected: dict[str, str], emitted_by_runner: dict[str, list[str]]
) -> None:
    observed: dict[str, str] = {}
    for runner, identifiers in emitted_by_runner.items():
        assert len(identifiers) == len(set(identifiers)), (
            f"{runner} emitted duplicate ledger assertion IDs"
        )
        for identifier in identifiers:
            assert identifier not in observed, (
                f"ledger assertion ID was emitted by multiple runners: {identifier}"
            )
            observed[identifier] = runner
    assert set(observed) == set(expected), (
        "executed ledger assertion coverage differs: missing="
        + ",".join(sorted(set(expected) - set(observed)))
        + " extra="
        + ",".join(sorted(set(observed) - set(expected)))
    )
    wrong = {
        identifier: (expected[identifier], observed[identifier])
        for identifier in expected
        if expected[identifier] != observed[identifier]
    }
    assert not wrong, f"ledger assertions were emitted by wrong runners: {wrong}"


def executed_coverage(build_dir: Path, runners: set[str]) -> dict[str, list[str]]:
    emitted: dict[str, list[str]] = {}
    for runner in sorted(runners):
        executable = build_dir / runner
        assert executable.is_file(), f"executed ledger evidence is unbuilt: {runner}"
        completed = subprocess.run(
            [str(executable), "--list-ledger-assertions"], cwd=ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        )
        assert completed.returncode == 0, (
            f"executed ledger evidence failed: {runner}\n{completed.stdout}"
        )
        try:
            payload = json.loads(completed.stdout.splitlines()[-1])
        except (IndexError, json.JSONDecodeError) as error:
            raise AssertionError(
                f"{runner} emitted no machine-readable ledger evidence"
            ) from error
        assert payload.get("runner") == runner, (
            f"{runner} emitted evidence for a different runner"
        )
        identifiers = payload.get("assertionIds")
        assert isinstance(identifiers, list) and all(
            isinstance(identifier, str) and identifier for identifier in identifiers
        ), f"{runner} emitted invalid assertion IDs"
        emitted[runner] = identifiers
    return emitted


def main() -> None:
    unexpected_arguments = set(sys.argv[1:]) - {"--require-complete"}
    assert not unexpected_arguments, (
        "unknown ledger-test arguments: " + ", ".join(sorted(unexpected_arguments))
    )
    require_complete = "--require-complete" in sys.argv[1:]

    # This catches a missing, added, or reclassified source feature before a
    # format implementation can accidentally leave it without an owner.
    assert EXTRACTOR_PATH.is_file(), "gameplay source-surface extractor must be committed"
    source_surface = load_json(SOURCE_SURFACE_PATH)
    ledger = load_json(LEDGER_PATH)

    for name, manifest in (("source surface", source_surface), ("ledger", ledger)):
        assert manifest["schemaVersion"] == SCHEMA_VERSION, (
            f"{name} schema must be version {SCHEMA_VERSION}"
        )
        assert manifest["pinnedCommit"] == PINNED_COMMIT, (
            f"{name} must use the pinned Beatoraja commit"
        )
        serialized = json.dumps(manifest).lower()
        assert not any(
            placeholder in serialized for placeholder in FORBIDDEN_CLASSIFICATIONS
        ), f"{name} contains a forbidden classification placeholder"

    source_features = source_surface["features"]
    ledger_features = ledger["features"]
    assert_sorted_ids(source_features, "source surface")
    assert_sorted_ids(ledger_features, "ledger")
    source_ids = assert_unique_ids(source_features, "source surface")
    ledger_ids = assert_unique_ids(ledger_features, "ledger")
    assert source_ids == ledger_ids, "source-surface and ledger IDs must match exactly"
    source_by_id = feature_by_id(source_features)
    ledger_by_id = feature_by_id(ledger_features)

    # These finite nested tables are part of the pinned legacy facade. A
    # shallow table.set() scan would omit their callable members.
    for identifier in {
        "lua.export.get-width",
        "lua.export.get-height",
        "lua.export.is-key-pressed",
        "lua.export.size",
        "lua.export.first",
        "lua.export.get-button",
        "lua.export.get-name",
    }:
        assert identifier in source_by_id, f"missing pinned Lua export {identifier}"

    # JSON Skin retains these root declarations, but the associated select and
    # configuration child object fields are not gameplay-loader surfaces.
    for identifier in {
        "json.field.skin-songlist",
        "json.field.skin-skin-select",
    }:
        assert identifier in source_by_id, f"missing gameplay root declaration {identifier}"
    for identifier in {
        "json.field.song-list-id",
        "json.field.skin-configuration-property-custom-bms",
        "lua.object-field.song-list-id",
        "lua.object-field.skin-configuration-property-custom-bms",
    }:
        assert identifier not in source_by_id, f"non-gameplay child leaked into source surface: {identifier}"

    # These representative rows were previously deferred. Keep them pinned to
    # concrete decoder/test evidence so completing the ledger cannot silently
    # reclassify an unimplemented feature.
    previously_deferred = {
        "lua.object-field.bpmgraph-id",
        "lua.object-field.judge-graph-id",
        "lua.object-field.timing-visualizer-id",
    }
    for identifier in previously_deferred:
        row = ledger_by_id[identifier]
        assert row == {
            "id": identifier,
            "status": "implemented",
            "implementation": "src/skin/beatoraja/LuaSkinTableDecoder.cpp",
            "tests": "tests/beatoraja_skin_model_tests.cpp",
            "assertion": {
                "runner": "beatoraja_skin_model_tests",
            },
        }, f"{identifier} must retain concrete implementation evidence"

    timing_distribution_rows = [
        row
        for row in ledger_features
        if row["id"].startswith("lua.object-field.timing-distribution-graph-")
    ]
    assert timing_distribution_rows, "timing-distribution graph must be inventoried"
    for row in timing_distribution_rows:
        assert row["status"] == "source-defined-noop", (
            f"{row['id']} must retain its gameplay source no-op"
        )
        assert row["source"] == {
            "path": "src/bms/player/beatoraja/skin/SkinTimingDistributionGraph.java",
            "symbol": "SkinTimingDistributionGraph.prepare",
        }, f"{row['id']} must cite the gameplay-disabled draw behavior"

    for feature in source_features:
        source = feature["source"]
        assert source["path"] and source["symbol"], (
            f"{feature['id']} must retain pinned source provenance"
        )

    for row in ledger_features:
        assert row["status"] in VALID_STATUS, f"{row['id']} has an invalid status"
        if row["status"] == "implemented":
            assert row.get("implementation") and row.get("tests"), (
                f"{row['id']} must identify implementation and tests"
            )
            for evidence_kind in ("implementation", "tests"):
                evidence_paths = [
                    value.strip()
                    for value in row[evidence_kind].split(";")
                    if value.strip()
                ]
                assert evidence_paths, (
                    f"{row['id']} must retain {evidence_kind} paths"
                )
                for evidence_path in evidence_paths:
                    assert (ROOT / evidence_path).is_file(), (
                        f"{row['id']} cites missing {evidence_kind} evidence: "
                        f"{evidence_path}"
                    )
            assertion = row.get("assertion")
            assert isinstance(assertion, dict), (
                f"{row['id']} must bind its implementation to an executed assertion"
            )
            assert set(assertion) == {"runner"}, (
                f"{row['id']} ledger assertion may only name its native runner"
            )
            assert assertion.get("runner") in TEST_RUNNERS.values(), (
                f"{row['id']} must name a runnable native assertion target"
            )
        elif row["status"] == "missing":
            assert row.get("plan", "").startswith("docs/superpowers/plans/"), (
                f"{row['id']} must have an owning plan"
            )
            assert row.get("task", "").startswith("Task "), (
                f"{row['id']} must have an owning task"
            )
        else:
            source = row.get("source", {})
            assert source.get("path") and source.get("symbol"), (
                f"{row['id']} must identify its source-defined no-op"
            )

    if require_complete:
        missing = [row["id"] for row in ledger_features if row["status"] == "missing"]
        assert not missing, (
            "complete gameplay ledger still contains missing rows: "
            + ", ".join(missing)
        )
        build_dir = Path(os.environ.get("ASOBMASHOW_TEST_BUILD_DIR", ROOT / "cmake-build-debug"))
        required_runners = {
            row["assertion"]["runner"]
            for row in ledger_features
            if row["status"] == "implemented"
        }
        emitted = executed_coverage(build_dir, required_runners)
        expected = {
            row["id"]: row["assertion"]["runner"]
            for row in ledger_features
            if row["status"] == "implemented"
        }
        validate_executed_coverage(expected, emitted)


if __name__ == "__main__":
    main()
