#!/usr/bin/env python3
"""Validate the pinned Beatoraja Lua music-select source contract."""

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BEATORAJA_ROOT = Path(
    os.environ.get(
        "ASOBMASHOW_BEATORAJA_ROOT",
        str(ROOT.parent / "beatoraja"),
    )
)
PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
SCHEMA_VERSION = 1
SOURCE_SURFACE_PATH = (
    ROOT / "docs/skin-compat/beatoraja-music-select-source-surface-v1.json"
)
LEDGER_PATH = (
    ROOT / "docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json"
)
EXTRACTOR_PATH = ROOT / "scripts/extract_beatoraja_music_select_skin_surface.py"
VALID_STATUS = {"implemented", "missing", "source-defined-noop"}
FORBIDDEN_PLACEHOLDERS = {"unclassified", "tbd", "todo"}


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def assert_unique_sorted(test: unittest.TestCase, rows: list[dict], label: str):
    identifiers = [row["id"] for row in rows]
    test.assertEqual(identifiers, sorted(identifiers), f"{label} must be sorted")
    test.assertEqual(len(identifiers), len(set(identifiers)), f"{label} IDs")
    return set(identifiers)


class MusicSelectSkinLedgerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(ROOT))
        from scripts import extract_beatoraja_music_select_skin_surface

        cls.extractor = extract_beatoraja_music_select_skin_surface

    def test_type5_surface_contains_songlist_and_selector_runtime(self):
        if not (BEATORAJA_ROOT / ".git").exists():
            self.skipTest("optional sibling Beatoraja checkout is unavailable")
        actual = subprocess.run(
            ["git", "-C", str(BEATORAJA_ROOT), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        self.assertEqual(actual, PINNED_COMMIT)

        surface = self.extractor.extract(BEATORAJA_ROOT)
        identifiers = {row["id"] for row in surface["features"]}
        for expected in {
            "lua.object-field.song-list-center",
            "lua.object-field.song-list-clickable",
            "select.skin-bar.bar-count",
            "select.property.boolean.music-selector",
            "select.property.integer.music-selector",
            "select.property.float.music-selector",
            "select.property.string.music-selector",
            "select.property.event.music-selector",
            "select.input.music-select-input-processor",
        }:
            self.assertIn(expected, identifiers)

    def test_committed_surface_and_ledger_are_exact_and_classified(self):
        surface = load_json(SOURCE_SURFACE_PATH)
        ledger = load_json(LEDGER_PATH)
        for label, manifest in (("surface", surface), ("ledger", ledger)):
            self.assertEqual(manifest["schemaVersion"], SCHEMA_VERSION, label)
            self.assertEqual(manifest["pinnedCommit"], PINNED_COMMIT, label)
            serialized = json.dumps(manifest).lower()
            for placeholder in FORBIDDEN_PLACEHOLDERS:
                self.assertNotIn(placeholder, serialized, label)

        source_ids = assert_unique_sorted(self, surface["features"], "surface")
        ledger_ids = assert_unique_sorted(self, ledger["features"], "ledger")
        self.assertEqual(source_ids, ledger_ids)

        for row in surface["features"]:
            self.assertTrue(row["source"]["path"], row["id"])
            self.assertTrue(row["source"]["symbol"], row["id"])
        for row in ledger["features"]:
            self.assertIn(row["status"], VALID_STATUS, row["id"])
            if row["status"] == "implemented":
                self.assertTrue(row.get("implementation"), row["id"])
                self.assertTrue(row.get("tests"), row["id"])
                self.assertEqual(set(row.get("assertion", {})), {"runner"})
            elif row["status"] == "missing":
                self.assertTrue(row.get("plan", "").startswith("docs/superpowers/plans/"))
                self.assertTrue(row.get("task", "").startswith("Task "))
            else:
                self.assertTrue(row.get("source", {}).get("path"), row["id"])
                self.assertTrue(row.get("source", {}).get("symbol"), row["id"])

    def test_committed_surface_matches_the_pinned_checkout(self):
        if not (BEATORAJA_ROOT / ".git").exists():
            self.skipTest("optional sibling Beatoraja checkout is unavailable")
        self.extractor.verify_commit(BEATORAJA_ROOT)
        self.assertEqual(load_json(SOURCE_SURFACE_PATH),
                         self.extractor.extract(BEATORAJA_ROOT))


if __name__ == "__main__":
    unittest.main()
