import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SNAPSHOTTER = ROOT / "src/skin/package/SkinTreeSnapshotter.cpp"
ALIAS_DETECTOR = ROOT / "src/skin/package/SkinAliasDetector.cpp"


class WindowsSkinTreeSnapshotContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.snapshotter = SNAPSHOTTER.read_text(encoding="utf-8")
        cls.alias_detector = ALIAS_DETECTOR.read_text(encoding="utf-8")

    def test_windows_source_walks_with_retained_no_follow_handles(self):
        source = self.snapshotter
        for token in (
            "class UniqueHandle",
            "CreateFileW(",
            "FILE_FLAG_OPEN_REPARSE_POINT",
            "FILE_FLAG_BACKUP_SEMANTICS",
            "FILE_SHARE_READ | FILE_SHARE_WRITE",
            "std::vector<UniqueHandle> handles",
            "openInventoryFileNoFollow",
        ):
            self.assertIn(token, source)
        self.assertNotIn("FILE_SHARE_DELETE", source)
        self.assertNotIn("GetFileAttributesW", self.alias_detector)
        self.assertIn("CreateFileW(", self.alias_detector)
        self.assertNotIn("FILE_SHARE_DELETE", self.alias_detector)

    def test_windows_metadata_is_captured_from_the_open_handle(self):
        source = self.snapshotter
        for token in (
            "GetFileInformationByHandleEx",
            "FileAttributeTagInfo",
            "GetFileInformationByHandle",
            "dwVolumeSerialNumber",
            "nFileIndexHigh",
            "nFileIndexLow",
            "nNumberOfLinks",
            "ftLastWriteTime",
            "nFileSizeHigh",
            "nFileSizeLow",
        ):
            self.assertIn(token, source)
        for path_metadata_call in (
            "fs::symlink_status",
            "fs::file_size",
            "fs::hard_link_count",
            "fs::last_write_time",
        ):
            self.assertNotIn(path_metadata_call, source)

    def test_windows_copy_uses_checked_handle_io_and_chunk_cancellation(self):
        source = self.snapshotter
        copy_source = source[source.index("digestAndMaybeCopy") :]
        windows_copy = re.search(
            r"#if defined\(_WIN32\)(.*?)#else\n    const int input =",
            copy_source,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(windows_copy)
        branch = windows_copy.group(1)
        self.assertIn("ReadFile(", branch)
        self.assertIn("WriteFile(", branch)
        self.assertIn("FlushFileBuffers(", branch)
        self.assertGreaterEqual(branch.count("stop.stop_requested()"), 2)
        self.assertIn("written != read", branch)
        self.assertNotIn("std::ifstream", branch)
        self.assertNotIn("std::ofstream", branch)

    def test_windows_sync_and_publication_are_not_success_stubs(self):
        source = self.snapshotter
        self.assertIn("FlushFileBuffers(", source)
        self.assertIn("MoveFileExW(", source)
        self.assertIn("MOVEFILE_WRITE_THROUGH", source)
        self.assertNotRegex(
            source,
            r"bool fsyncDirectory\(const fs::path &\) \{ return true; \}",
        )
        self.assertNotRegex(
            source,
            r"bool fsyncDirectoryTree\(const fs::path &\) \{ return true; \}",
        )


if __name__ == "__main__":
    unittest.main()
