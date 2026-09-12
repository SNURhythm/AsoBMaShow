from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from find_bms_transport_extract import function


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "darwin", "Requires macOS DYLD interposition and funopen")
class UnzipMarkerIOTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get("CXX", "clang++"))
        if not compiler:
            raise unittest.SkipTest("C++ compiler unavailable")
        cls.output = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.output.cleanup)
        output = Path(cls.output.name)
        source = (ROOT / "src/ArchiveFile.cpp").read_text()
        pieces = [function(source, signature) for signature in (
            "std::filesystem::path cacheNormalizedPath(",
            "bool unzipFolderMarkerMatches(",
            "bool unzipFolderHasMatchingIncompleteMarker(",
            "bool unzipFolderHasMatchingCompleteMarker(",
        )]
        (output / "unzip_marker_methods.inc").write_text("\n\n".join(pieces))
        cls.executable = output / "marker-test"
        cls.interposer = output / "marker-io.dylib"
        common = [compiler, "-std=c++20", "-stdlib=libc++"]
        subprocess.run(common + ["-I", str(ROOT / "src"), "-I", str(output),
                                str(ROOT / "tests/unzip_marker_io_fixture.cpp"),
                                str(ROOT / "src/path.cpp"), "-o", str(cls.executable)],
                       check=True)
        subprocess.run(common + ["-dynamiclib",
                                str(ROOT / "tests/unzip_marker_io_interpose_fixture.cpp"),
                                "-o", str(cls.interposer)], check=True)

    def run_marker(self, contents, expected_match=False, fail_after=None, complete=False):
        with tempfile.TemporaryDirectory(dir=self.output.name) as directory:
            folder = Path(directory)
            source = folder / "source.zip"
            marker = folder / (".asobmashow_unzip_complete" if complete
                               else ".asobmashow_unzip_incomplete")
            marker.write_bytes(contents.replace(b"SOURCE", str(source).encode()))
            environment = os.environ.copy()
            environment.pop("ASOBMSHOW_MARKER_IO_PATH", None)
            environment.pop("ASOBMSHOW_MARKER_IO_AFTER", None)
            environment.pop("DYLD_INSERT_LIBRARIES", None)
            if fail_after is not None:
                environment.update(DYLD_INSERT_LIBRARIES=str(self.interposer),
                                   ASOBMSHOW_MARKER_IO_PATH=str(marker),
                                   ASOBMSHOW_MARKER_IO_AFTER=str(fail_after))
            result = subprocess.run(
                [str(self.executable), str(folder), str(source),
                 "complete" if complete else "incomplete",
                 "1" if expected_match else "0",
                 "1" if fail_after is not None and not complete else "0"],
                env=environment, capture_output=True, text=True, timeout=10)
            diagnostic = f"exit={result.returncode}\n" + result.stdout + result.stderr
            print(diagnostic, end="", flush=True)
            if fail_after is not None:
                self.assertIn("injected marker read EIO", result.stderr, diagnostic)
            self.assertEqual(result.returncode, 0, diagnostic)

    def test_read_failure_is_not_a_readable_mismatch(self):
        for fail_after in (0, 4, 7):
            with self.subTest(fail_after=fail_after):
                self.run_marker(b"key\nSOURCE\n", fail_after=fail_after)

    def test_readable_markers_clear_errors(self):
        for contents, matches in (
            (b"key\nSOURCE\n", True),
            (b"key\nSOURCE", True),
            (b"", False),
            (b"key\n", False),
            (b"wrong-key\nSOURCE\n", False),
            (b"key\n/other-source.zip\n", False),
            (b"x" * (64 * 1024) + b"\nSOURCE\n", False),
            (b"key\n" + b"x" * (64 * 1024) + b"\n", False),
        ):
            with self.subTest(prefix=contents[:24], size=len(contents)):
                self.run_marker(contents, expected_match=matches)

    def test_complete_validator_keeps_boolean_contract(self):
        self.run_marker(b"key\nSOURCE\n", expected_match=True, complete=True)
        self.run_marker(b"wrong-key\nSOURCE\n", complete=True)
        self.run_marker(b"key\nSOURCE\n", fail_after=0, complete=True)


if __name__ == "__main__":
    unittest.main()
