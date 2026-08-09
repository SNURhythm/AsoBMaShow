import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
IMPORTER = ROOT / "src/skin/package/SkinArchiveImporter.cpp"


class WindowsSkinArchiveImporterContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = IMPORTER.read_text(encoding="utf-8")

    def test_issued_root_handle_is_never_inferred_from_ancestry(self):
        source = self.source
        self.assertIn("HANDLE issuedRootHandle_ = INVALID_HANDLE_VALUE;", source)
        self.assertNotRegex(source, r"rootHandle\(\).*?ancestryHandles_\.back\(\)")
        self.assertNotIn("ancestryHandles_.push_back(issued", source)
        cleanup = source[source.index("void cleanup() noexcept") :]
        self.assertIn("issuedRootHandle_ == INVALID_HANDLE_VALUE", cleanup)
        self.assertIn("closeWindowsHandles(ancestryHandles_);", cleanup)
        self.assertIn("markWindowsDeletion(issuedRootHandle_)", cleanup)

    def test_partial_windows_handle_chains_close_on_every_failure(self):
        source = self.source
        chain = source[
            source.index("openExistingAbsoluteWindowsDirectoryChain") :
            source.index("bool openWindowsDirectories")
        ]
        self.assertIn("closeWindowsHandles(handles);", chain)
        self.assertGreaterEqual(chain.count("return false;"), 3)
        create_directory = source[
            source.index("createDirectory(std::string_view relative") :
            source.index("HANDLE createFile(std::string_view relative")
        ]
        failure_tail = create_directory[
            create_directory.index("#elif defined(_WIN32)") :
        ]
        self.assertGreaterEqual(failure_tail.count("closeWindowsHandles(opened);"), 2)

    def test_windows_staging_objects_use_and_validate_private_security(self):
        source = self.source
        for token in (
            "class PrivateWindowsSecurity",
            "OpenProcessToken(",
            "GetTokenInformation(",
            "SetEntriesInAclW(",
            "SetSecurityDescriptorDacl(",
            "SetSecurityDescriptorControl(",
            "SE_DACL_PROTECTED",
            "OWNER_SECURITY_INFORMATION",
            "GetSecurityInfo(",
            "GetSecurityDescriptorControl(",
            "GetAce(",
            "EqualSid(",
            "security_.attributes()",
            "security_.verify(",
            "READ_CONTROL",
        ):
            self.assertIn(token, source)
        self.assertRegex(
            source,
            r"CreateDirectoryW\([^;]+security_\.attributes\(\)\)",
        )
        self.assertRegex(
            source,
            r"CreateFileW\([^;]+security_\.attributes\(\)[^;]+CREATE_NEW",
        )
        allocate = source[
            source.index("bool allocate(const fs::path &parentPath") :
            source.index("void cleanup() noexcept")
        ]
        self.assertIn("fs::create_directories(parentPath.parent_path()", allocate)
        self.assertIn("parentPath.parent_path(),", allocate)
        self.assertIn("CreateDirectoryW(parentPath_.c_str(),", allocate)
        self.assertIn("security_.verify(stagingParent, true)", allocate)
        ancestry_chain = source[
            source.index("openExistingAbsoluteWindowsDirectoryChain") :
            source.index("bool openWindowsDirectories")
        ]
        self.assertIn("FILE_TRAVERSE", ancestry_chain)
        self.assertNotIn("CreateDirectoryW(", ancestry_chain)

    def test_windows_build_uses_libarchive_types_and_const_security_access(self):
        source = self.source
        self.assertNotRegex(source, r"\bmode_t\b")
        self.assertIn(
            "SECURITY_ATTRIBUTES *attributes() const noexcept", source
        )

    def test_existing_windows_staging_parent_is_hardened_through_its_handle(self):
        source = self.source
        self.assertIn("bool protect(HANDLE handle) const", source)
        self.assertRegex(source, r"SetSecurityInfo\(\s*handle,\s*SE_FILE_OBJECT")
        self.assertIn("PROTECTED_DACL_SECURITY_INFORMATION", source)
        allocate = source[
            source.index("bool allocate(const fs::path &parentPath") :
            source.index("void cleanup() noexcept")
        ]
        self.assertIn("WRITE_DAC", allocate)
        self.assertIn("security_.protect(stagingParent)", allocate)

    def test_windows_root_and_recursive_cleanup_handles_pin_names(self):
        source = self.source
        issued_open = re.search(
            r"issuedRootHandle_\s*=\s*CreateFileW\((.*?)\);",
            source,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(issued_open)
        self.assertNotIn("FILE_SHARE_DELETE", issued_open.group(1))
        recursive_cleanup = source[
            source.index("clearWindowsDirectory") :
            source.index("#endif", source.index("clearWindowsDirectory"))
        ]
        self.assertNotIn("FILE_SHARE_DELETE", recursive_cleanup)

    def test_failed_issued_root_allocation_never_falls_back_to_path_deletion(self):
        source = self.source
        allocate = source[
            source.index("bool allocate(const fs::path &parentPath") :
            source.index("void cleanup() noexcept")
        ]
        self.assertNotIn("RemoveDirectoryW(", allocate)
        self.assertNotRegex(
            allocate,
            r"CloseHandle\(issuedRootHandle_\).*RemoveDirectoryW",
        )
        self.assertIn("safeIssuedDirectory", allocate)
        self.assertIn("markWindowsDeletion(issuedRootHandle_)", allocate)
        self.assertLess(
            allocate.index("markWindowsDeletion(issuedRootHandle_)"),
            allocate.index("CloseHandle(issuedRootHandle_)"),
        )
        cleanup = source[
            source.index("void cleanup() noexcept") :
            source.index("fs::path path_", source.index("void cleanup() noexcept"))
        ]
        guard = cleanup.index("issuedRootHandle_ == INVALID_HANDLE_VALUE")
        path_cleanup = cleanup.index("clearWindowsDirectory(path_)")
        self.assertLess(guard, path_cleanup)

    def test_windows_reopened_payload_is_regular_nofollow_single_link(self):
        source = self.source
        branch = source[
            source.index("HANDLE openFileForRead") :
            source.index("#else", source.index("HANDLE openFileForRead"))
        ]
        for token in (
            "FILE_FLAG_OPEN_REPARSE_POINT",
            "FileAttributeTagInfo",
            "FILE_ATTRIBUTE_REPARSE_POINT",
            "FILE_ATTRIBUTE_DIRECTORY",
            "GetFileInformationByHandle(",
            "nNumberOfLinks == 1",
        ):
            self.assertIn(token, branch)

        posix_branch = source[
            source.index("int openFileForRead") :
            source.index("#endif", source.index("int openFileForRead"))
        ]
        self.assertIn("O_NONBLOCK", posix_branch)

    def test_flush_failure_does_not_skip_handle_close(self):
        source = self.source
        self.assertNotIn("FlushFileBuffers(output) && CloseHandle(output)", source)
        self.assertNotIn("::fsync(output) == 0 && ::close(output) == 0", source)

    def test_rename_buffer_includes_the_complete_variable_length_structure(self):
        self.assertNotIn("offsetof(FILE_RENAME_INFO, FileName)", self.source)
        self.assertIn("sizeof(FILE_RENAME_INFO) + leafBytes", self.source)
        self.assertIn("rename->RootDirectory = nullptr", self.source)


if __name__ == "__main__":
    unittest.main()
