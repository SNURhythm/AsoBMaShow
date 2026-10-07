"""Exercise import destination reservations against real host filesystem paths."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
JAVA_ROOT = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow"


class DocumentsMutationGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(
                ["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        cls.java = str(Path(java_home) / "bin/java") if java_home else "java"
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        cls.output = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.output.cleanup)
        subprocess.run([
            javac, "-d", cls.output.name,
            str(JAVA_ROOT / "DocumentsMutationGuard.java"),
            str(ROOT / "tests/java/DocumentsMutationGuardTests.java"),
        ], check=True)

    def run_scenario(self, scenario):
        subprocess.run([
            self.java, "-cp", self.output.name,
            "com.snurhythm.asobmashow.DocumentsMutationGuardTests", scenario,
        ], check=True, timeout=15)

    def test_existing_files_directories_and_dangling_symlinks_are_rejected(self):
        self.run_scenario("existing")

    def test_destination_and_descendants_are_reserved_before_creation(self):
        self.run_scenario("descendants")

    def test_ancestor_removal_is_blocked_but_sibling_creation_is_allowed(self):
        self.run_scenario("ancestors")

    def test_canonical_aliases_and_overlapping_reservations_are_rejected(self):
        self.run_scenario("overlap")

    def test_case_aliases_cannot_bypass_reservations_or_ancestor_protection(self):
        self.run_scenario("case-aliases")

    def test_close_and_exception_unwinding_release_only_the_owned_reservation(self):
        self.run_scenario("release")

    def test_every_operation_uses_the_shared_mutation_monitor(self):
        self.run_scenario("monitor")

    def test_concurrent_imports_cannot_reserve_the_same_destination(self):
        self.run_scenario("concurrent")


if __name__ == "__main__":
    unittest.main()
