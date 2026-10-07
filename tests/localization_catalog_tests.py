"""Validate stable message IDs and their references without requiring the UI."""

from collections import Counter
import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
KEY = re.compile(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*){2,}")
LITERAL = r'"(?:[^"\\]|\\.)*"'


class LocalizationCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.entries = [
            json.loads("[" + line[1:-2] + "]")
            for line in (ROOT / "src/i18n/Messages.inc").read_text().splitlines()
            if line.startswith("{")
        ]
        cls.catalog = {entry[0]: tuple(entry[1:]) for entry in cls.entries}

    def test_keys_are_unique_structural_and_sorted(self):
        keys = [entry[0] for entry in self.entries]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertEqual(keys, sorted(keys))
        for key in keys:
            self.assertRegex(key, "^" + KEY.pattern + "$")

    def test_languages_preserve_named_placeholders(self):
        for key, *values in self.entries:
            with self.subTest(key=key):
                self.assertEqual(len(values), 5, "English, Korean, Japanese, Simplified Chinese, and Traditional Chinese are required")
                english = values[0]
                for translated in values:
                    self.assertTrue(translated)
                    self.assertEqual(Counter(re.findall(r"\{(\w+)\}", english)),
                                     Counter(re.findall(r"\{(\w+)\}", translated)))

    def test_source_uses_catalog_ids_and_every_message_is_referenced(self):
        referenced = set()
        sources = list((ROOT / "src").rglob("*"))
        sources.extend((ROOT / "android/app/src/main/java").rglob("*.java"))
        for path in sources:
            if path.suffix not in (".h", ".cpp", ".java"):
                continue
            source = path.read_text()
            for literal in re.findall(LITERAL, source):
                value = literal[1:-1]
                if value in self.catalog:
                    referenced.add(value)
            for match in re.finditer(r"(?:i18n::(?:tr|format|message)|nativeArchiveImportText)\(\s*(" + LITERAL + ")", source):
                key = json.loads(match[1])
                self.assertTrue(key in self.catalog, f"{path}: unknown message ID {key!r}")
        self.assertEqual(set(self.catalog) - referenced, set())

if __name__ == "__main__":
    unittest.main()
