import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FILESYSTEM = ROOT / "src/skin/beatoraja/LuaSkinFileSystem.cpp"
CMAKE = ROOT / "CMakeLists.txt"


class WindowsLuaSkinFileSystemContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = FILESYSTEM.read_text(encoding="utf-8")
        cls.cmake = CMAKE.read_text(encoding="utf-8")

    def test_windows_walk_retains_checked_no_reparse_ancestors(self):
        source = self.source
        for token in (
            "class UniqueWindowsHandle",
            "struct WindowsHandleChain",
            "openWindowsDirectoryChain",
            "std::vector<UniqueWindowsHandle> handles",
            "CreateFileW(",
            "FILE_FLAG_OPEN_REPARSE_POINT",
            "FILE_FLAG_BACKUP_SEMANTICS",
            "FILE_SHARE_READ | FILE_SHARE_WRITE",
        ):
            self.assertIn(token, source)
        self.assertNotIn("FILE_SHARE_DELETE", source)

    def test_windows_metadata_and_reads_use_the_retained_handle(self):
        source = self.source
        for token in (
            "GetFileType(handle) == FILE_TYPE_DISK",
            "GetFileInformationByHandleEx(handle, FileIdInfo",
            "GetFileInformationByHandle(handle, &information)",
            "information.nNumberOfLinks != 1",
            "sameWindowsIdentity",
            "ReadFile(",
        ):
            self.assertIn(token, source)
        windows_branch = source[
            source.index("class UniqueWindowsHandle") :
            source.index("#endif", source.index("class UniqueWindowsHandle"))
        ]
        self.assertNotIn("std::ifstream", windows_branch)

    def test_windows_enumeration_uses_the_retained_directory_handle(self):
        source = self.source
        for token in (
            "FileIdExtdDirectoryRestartInfo",
            "FILE_ID_EXTD_DIR_INFO",
            "FILE_ATTRIBUTE_REPARSE_POINT",
            "FileIdExtdDirectoryInfo",
            "FILE_ID_128 fileId",
            "sameWindowsEnumeratedIdentity",
            "entry.fileId",
            "metadata.identity.FileId.Identifier",
            "metadata.identity.VolumeSerialNumber",
            "entry.volumeSerial",
        ):
            self.assertIn(token, source)
        self.assertNotIn("FILE_ID_BOTH_DIR_INFO", source)
        self.assertNotIn("FindFirstFileW", source)
        self.assertNotIn("FindNextFileW", source)

    def test_windows_overlay_creation_and_replace_keep_the_parent_capability(self):
        source = self.source
        for token in (
            "class PrivateWindowsSecurity",
            "SECURITY_ATTRIBUTES",
            "GetSecurityInfo(",
            "security.verify(",
            "secureReplaceAtWindowsParent",
            "SetFileInformationByHandle(",
            "FileRenameInfo",
            "rename.RootDirectory = parent",
            "secureReplaceOverlayFile",
            "renameat(",
            "openat(",
            "verifyPrivateWindowsSecurity(chain->leaf())",
            "verifyPrivateWindowsSecurity(child.get())",
            "acquireWindowsOverlayMutationPin",
            "InternalTemporaryCleanup",
            "recoverWindowsOwnedTemporaries",
            "internalTemporaryDirectoryName",
        ):
            self.assertIn(token, source)
        self.assertRegex(source, r"FILE_READ_ATTRIBUTES\s*\|\s*READ_CONTROL")
        usage = source[
            source.index("bool collectUsageFromWindowsDirectory") :
            source.index(
                "listAtRoot",
                source.index("bool collectUsageFromWindowsDirectory"),
            )
        ]
        self.assertGreaterEqual(usage.count("READ_CONTROL"), 2)
        self.assertNotIn("atomic_file::writeWithoutBackup", source)
        self.assertIn("maximumTemporaryCreateAttempts", source)
        self.assertIn("uniqueOverlayTemporaryName", source)
        self.assertIn("markWindowsDeletion", source)
        replacement = source[
            source.index("secureReplaceAtWindowsParent") :
            source.index(
                "bool secureReplaceOverlayFile",
                source.index("secureReplaceAtWindowsParent"),
            )
        ]
        self.assertNotIn("MoveFileExW", replacement)
        self.assertNotIn("AtomicFile", replacement)
        mkdir = source[
            source.index("LuaSkinFileSystem::mkdirData") :
            source.index(
                "LuaSkinFileSystem::enterRenderPhase",
                source.index("LuaSkinFileSystem::mkdirData"),
            )
        ]
        self.assertIsNone(
            re.search(
                r"secureCreateOverlayDirectory\(.*?statAtRoot",
                mkdir,
                flags=re.DOTALL,
            )
        )
        write = source[
            source.index("LuaSkinFileSystem::writeData") :
            source.index(
                "LuaSkinFileSystem::mkdirData",
                source.index("LuaSkinFileSystem::writeData"),
            )
        ]
        self.assertLess(
            write.index("acquireWindowsOverlayMutationPin"),
            write.index("collectUsage"),
        )
        self.assertLess(
            mkdir.index("acquireWindowsOverlayMutationPin"),
            mkdir.index("collectUsage"),
        )

    def test_existing_windows_overlay_privacy_is_checked_on_create_and_read(self):
        source = self.source
        for token in (
            "validatePrivateOverlayRoot",
            "readAtPrivateOverlay",
            "verifyPrivateWindowsSecurity(chain->leaf())",
        ):
            self.assertIn(token, source)
        self.assertRegex(
            source,
            r"FILE_LIST_DIRECTORY\s*\|\s*FILE_READ_ATTRIBUTES\s*\|\s*READ_CONTROL",
        )
        create = source[
            source.index("LuaSkinFileSystem::create") :
            source.index("LuaSkinFileSystem::LuaSkinFileSystem", source.index("LuaSkinFileSystem::create"))
        ]
        self.assertIn("validatePrivateOverlayRoot", create)

    def test_windows_test_target_links_the_security_api(self):
        target = self.cmake[
            self.cmake.index("add_executable(lua_skin_file_system_tests") :
            self.cmake.index(
                "function(asobmashow_register_test",
                self.cmake.index("add_executable(lua_skin_file_system_tests"),
            )
        ]
        self.assertIn("advapi32", target)


if __name__ == "__main__":
    unittest.main()
