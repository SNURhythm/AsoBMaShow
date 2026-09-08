#!/usr/bin/env python3
"""Regression tests for runner-owned music-select ledger evidence."""

import json
import os
import subprocess
import unittest
from pathlib import Path

from tests import beatoraja_music_select_skin_ledger_tests as ledger


TEST_RUNNERS = {
    "tests/beatoraja_skin_model_tests.cpp": "beatoraja_skin_model_tests",
    "tests/gameplay_skin_traits_tests.cpp": "gameplay_skin_traits_tests",
    "tests/json_gameplay_skin_decoder_tests.cpp":
        "json_gameplay_skin_decoder_tests",
    "tests/lua_skin_file_system_tests.cpp": "lua_skin_file_system_tests",
    "tests/lua_skin_host_modules_tests.cpp": "lua_skin_host_modules_tests",
    "tests/lua_skin_runtime_tests.cpp": "lua_skin_runtime_tests",
    "tests/lua_music_select_skin_decoder_tests.cpp":
        "lua_music_select_skin_decoder_tests",
    "tests/lua_skin_text_graph_live_integration_tests.cpp":
        "lua_skin_text_graph_live_integration_tests",
    "tests/music_select_input_processor_tests.cpp":
        "music_select_input_processor_tests",
    "tests/music_select_bar_manager_tests.cpp":
        "music_select_bar_manager_tests",
    "tests/music_select_bar_renderer_tests.cpp":
        "music_select_bar_renderer_tests",
    "tests/music_select_event_controller_tests.cpp":
        "music_select_event_controller_tests",
    "tests/music_select_preview_tests.cpp": "music_select_preview_tests",
    "tests/music_select_property_projection_tests.cpp":
        "music_select_property_projection_tests",
    "tests/music_select_ranking_tests.cpp": "music_select_ranking_tests",
    "tests/music_select_repository_projection_tests.cpp":
        "music_select_repository_projection_tests",
    "tests/music_select_search_history_tests.cpp":
        "music_select_search_history_tests",
    "tests/music_select_skin_model_tests.cpp":
        "music_select_skin_model_tests",
    "tests/music_select_skin_state_bridge_tests.cpp":
        "music_select_skin_state_bridge_tests",
    "tests/play_skin_session_tests.cpp": "play_skin_session_tests",
    "tests/skin_draw_command_tests.cpp": "skin_draw_command_tests",
}


def validate_executed_coverage(expected, emitted_by_runner):
    observed = {}
    for runner, identifiers in emitted_by_runner.items():
        assert len(identifiers) == len(set(identifiers)), (
            f"{runner} emitted duplicate ledger IDs"
        )
        for identifier in identifiers:
            assert identifier not in observed, (
                f"ledger ID emitted by multiple runners: {identifier}"
            )
            observed[identifier] = runner
    assert set(observed) == set(expected), (
        "executed ledger coverage differs: missing="
        + ",".join(sorted(set(expected) - set(observed)))
        + " extra="
        + ",".join(sorted(set(observed) - set(expected)))
    )
    assert all(observed[key] == expected[key] for key in expected), (
        "ledger IDs were emitted by the wrong runner"
    )


def executed_coverage(build_dir: Path, runners: set[str]):
    emitted = {}
    for runner in sorted(runners):
        executable = build_dir / runner
        assert executable.is_file(), f"ledger evidence is unbuilt: {runner}"
        completed = subprocess.run(
            [str(executable), "--list-ledger-assertions"],
            cwd=ledger.ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        assert completed.returncode == 0, completed.stdout
        payload = json.loads(completed.stdout.splitlines()[-1])
        assert payload.get("runner") == runner
        identifiers = payload.get("assertionIds")
        assert isinstance(identifiers, list)
        assert all(isinstance(identifier, str) and identifier
                   for identifier in identifiers)
        emitted[runner] = identifiers
    return emitted


class MusicSelectSkinLedgerEvidenceTests(unittest.TestCase):
    def test_implemented_rows_have_existing_owned_evidence(self):
        manifest = ledger.load_json(ledger.LEDGER_PATH)
        expected = {}
        for row in manifest["features"]:
            if row["status"] != "implemented":
                continue
            for field in ("implementation", "tests"):
                for value in row[field].split(";"):
                    self.assertTrue((ledger.ROOT / value.strip()).is_file(), row["id"])
            runner = row["assertion"]["runner"]
            self.assertIn(runner, TEST_RUNNERS.values(), row["id"])
            owned_runners = {
                TEST_RUNNERS.get(path.strip())
                for path in row["tests"].split(";")
            }
            self.assertIn(runner, owned_runners, row["id"])
            expected[row["id"]] = runner

        build_dir = Path(os.environ.get(
            "ASOBMASHOW_TEST_BUILD_DIR", ledger.ROOT / "cmake-build-debug"
        ))
        emitted = executed_coverage(build_dir, set(expected.values()))
        music_select_ids = {
            row["id"] for row in manifest["features"]
        }
        emitted = {
            runner: [
                identifier for identifier in identifiers
                if identifier in music_select_ids
            ]
            for runner, identifiers in emitted.items()
        }
        validate_executed_coverage(expected, emitted)

    def test_exact_coverage_rejects_missing_extra_duplicate_and_wrong_runner(self):
        expected = {"select.target.music-select": "gameplay_skin_traits_tests"}
        validate_executed_coverage(
            expected,
            {"gameplay_skin_traits_tests": ["select.target.music-select"]},
        )
        for emitted in (
            {"gameplay_skin_traits_tests": []},
            {"gameplay_skin_traits_tests": [
                "select.target.music-select", "select.target.extra"
            ]},
            {"gameplay_skin_traits_tests": [
                "select.target.music-select", "select.target.music-select"
            ]},
            {"another_runner": ["select.target.music-select"]},
        ):
            with self.subTest(emitted=emitted), self.assertRaises(AssertionError):
                validate_executed_coverage(expected, emitted)


if __name__ == "__main__":
    unittest.main()
