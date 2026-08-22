#!/usr/bin/env python3
"""Contract tests for revision-honest gameplay-skin benchmarks."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/benchmark_gameplay_skin_loading.sh"


class GameplaySkinLoadingBenchmarkScriptTests(unittest.TestCase):
    def test_runner_never_injects_candidate_sources_into_baseline(self):
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertNotIn('cp "$candidate_tree/tests/gameplay_skin_loading_benchmark_tests.cpp"', source)
        self.assertNotIn('path.write_text(path.read_text', source)
        self.assertIn("same benchmark target/workload is unavailable", source)

    def test_runner_compares_each_supported_format_without_forcing_lua(self):
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertNotIn("extra=(--format lua)", source)
        self.assertIn("for format in lua json lr2", source)
        self.assertIn('"$format"', source)


if __name__ == "__main__":
    unittest.main()
