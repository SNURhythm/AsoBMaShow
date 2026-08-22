#!/usr/bin/env python3
"""Validate the committed Beatoraja gameplay skin parity contract."""

import json
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


def main() -> None:
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

    # These current Aso paths deliberately fail closed or are deferred; a
    # default implemented classification would hide a source-defined gap.
    expected_gaps = {
        "lua.object-field.bpmgraph-id": "Task 6: BPM/scroll/stop graph",
        "lua.object-field.judge-graph-id": "Task 2: Judgement and note-distribution graph",
        "lua.object-field.timing-visualizer-id": "Task 3: Timing visualizer",
    }
    for identifier, task in expected_gaps.items():
        row = ledger_by_id[identifier]
        assert row["status"] == "missing", f"{identifier} must not default to implemented"
        assert row["task"] == task, f"{identifier} must retain its owning task"

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


if __name__ == "__main__":
    main()
