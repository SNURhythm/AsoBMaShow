"""Validate stable message IDs and their references without requiring the UI."""

from collections import Counter
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
KEY = re.compile(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*){2,}")
LITERAL = r'"(?:[^"\\]|\\.)*"'


class TranslationKeyHookTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="translation-hook-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.environment = {key: value for key, value in os.environ.items()
                            if key not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE")}
        self.git("init", "--quiet")
        self.git("config", "user.name", "Hook test")
        self.git("config", "user.email", "hook-test@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.git("config", "core.autocrlf", "false")
        self.git("config", "core.hooksPath", str(ROOT / ".githooks"))
        self.catalog = self.root / "src/i18n/Messages.inc"
        self.catalog.parent.mkdir(parents=True)
        self.header = b"// Keep the catalog header.\n"
        self.a = b'{"test.alpha.label", "Alpha", "A", "A", "A", "A"},\n'
        self.z = b'{"test.zulu.label", "Zulu", "Z", "Z", "Z", "Z"},\n'

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root,
                                       env=self.environment, stderr=subprocess.STDOUT)

    def test_commit_sorts_staged_catalog_and_working_copy(self):
        self.catalog.write_bytes(self.header + self.z + self.a)
        self.git("add", "src/i18n/Messages.inc")
        self.git("commit", "--quiet", "-m", "Add catalog")
        expected = self.header + self.a + self.z
        self.assertEqual(self.git("show", "HEAD:src/i18n/Messages.inc"), expected)
        self.assertEqual(self.catalog.read_bytes(), expected)
        self.assertEqual(self.git("status", "--porcelain"), b"")

    def test_partial_staging_keeps_unstaged_translation_edits_out_of_commit(self):
        self.catalog.write_bytes(self.header + self.z + self.a)
        self.git("add", "src/i18n/Messages.inc")
        local_a = self.a.replace(b'"Alpha"', '"Unstaged 한국어"'.encode())
        self.catalog.write_bytes(self.header + self.z + local_a)
        self.git("commit", "--quiet", "-m", "Stage catalog only")
        self.assertEqual(self.git("show", "HEAD:src/i18n/Messages.inc"),
                         self.header + self.a + self.z)
        self.assertEqual(self.catalog.read_bytes(), self.header + local_a + self.z)
        self.assertEqual(self.git("diff", "--cached"), b"")
        self.assertIn(b"Unstaged", self.git("diff"))

    def test_unrelated_commit_does_not_touch_unstaged_catalog(self):
        self.catalog.write_bytes(self.header + self.a + self.z)
        self.git("add", ".")
        self.git("commit", "--quiet", "-m", "Initial catalog")
        unsorted = self.header + self.z + self.a
        self.catalog.write_bytes(unsorted)
        (self.root / "other.txt").write_text("Unrelated change\n")
        self.git("add", "other.txt")
        self.git("commit", "--quiet", "-m", "Other file")
        self.assertEqual(self.catalog.read_bytes(), unsorted)


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
            for match in re.finditer(r"(?:i18n::(?:tr|format|message|plural)|nativeArchiveImportText)\(\s*(" + LITERAL + ")", source):
                key = json.loads(match[1])
                self.assertTrue(key in self.catalog, f"{path}: unknown message ID {key!r}")
            for match in re.finditer(r"i18n::plural\(\s*(" + LITERAL + ")", source):
                key = json.loads(match[1]) + ".one"
                self.assertIn(key, self.catalog, f"{path}: missing singular form {key!r}")
                referenced.add(key)
        self.assertEqual(set(self.catalog) - referenced, set())

if __name__ == "__main__":
    unittest.main()
