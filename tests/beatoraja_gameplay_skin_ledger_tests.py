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
    source_ids = assert_unique_ids(source_features, "source surface")
    ledger_ids = assert_unique_ids(ledger_features, "ledger")
    assert source_ids == ledger_ids, "source-surface and ledger IDs must match exactly"

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
