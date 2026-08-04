import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
STORE = ROOT / "src/skin/package/SkinPackageStore.cpp"


class WindowsSkinPackageStoreContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = STORE.read_text(encoding="utf-8")
        cls.entry = cls.source[
            cls.source.index("class RetainedEntryCapability") :
            cls.source.index("bool renameTreeNoReplace")
        ]

    def test_regular_entry_handle_pins_identity_without_delete_sharing(self):
        issue = self.entry[
            self.entry.index("static std::optional<RetainedEntryCapability> issue") :
            self.entry.index("bool matchesIssuedIdentity")
        ]
        for token in (
            "DELETE | FILE_READ_ATTRIBUTES",
            "FILE_FLAG_OPEN_REPARSE_POINT",
            "FileAttributeTagInfo",
            "FILE_ATTRIBUTE_REPARSE_POINT",
            "FILE_ATTRIBUTE_DIRECTORY",
            "GetFileInformationByHandle(",
            "dwVolumeSerialNumber",
            "nFileIndexHigh",
            "GetFileType(file) != FILE_TYPE_DISK",
        ):
            self.assertIn(token, issue)
        file_open = issue[issue.index("HANDLE file = CreateFileW") :]
        self.assertNotIn("FILE_SHARE_DELETE", file_open)

    def test_windows_directory_chain_validates_root_and_every_component(self):
        chain = self.entry[self.entry.index("static bool openDirectoryChain") :]
        for token in (
            "FILE_FLAG_OPEN_REPARSE_POINT",
            "GetFileType(root) != FILE_TYPE_DISK",
            "FileAttributeTagInfo, &rootTags",
            "rootTags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT",
            "rootTags.FileAttributes & FILE_ATTRIBUTE_DIRECTORY",
            "tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT",
            "tags.FileAttributes & FILE_ATTRIBUTE_DIRECTORY",
        ):
            self.assertIn(token, chain)

    def test_handle_rename_validates_leaf_and_never_replaces(self):
        rename = self.entry[
            self.entry.index("bool renameTo") : self.entry.index("bool removeExact")
        ]
        for token in (
            "destinationLeaf.empty()",
            'destinationLeaf == "."',
            'destinationLeaf == ".."',
            "destinationLeafNative.find(L'/')",
            "destinationLeafNative.find(L'\\\\')",
            "rename->ReplaceIfExists = FALSE",
            "SetFileInformationByHandle(file_, FileRenameInfo",
            "path_ = destination",
            "return matchesIssuedIdentity()",
        ):
            self.assertIn(token, rename)

    def test_regular_files_delete_by_retained_handle_and_cleanup_is_typed(self):
        remove = self.entry[self.entry.index("bool removeExact") :]
        self.assertIn("SetFileInformationByHandle(file_, FileDispositionInfo", remove)
        self.assertIn("return tree_->removeTreeExact()", remove)
        rename_dispatch = self.source[
            self.source.index("bool renameTreeNoReplace") :
            self.source.index("std::string nextOperationId();")
        ]
        self.assertIn("RetainedEntryCapability::issue(source)", rename_dispatch)
        cleanup = self.source[
            self.source.index("void cleanupOwnedEntries") :
            self.source.index("bool writeJournal")
        ]
        self.assertIn("fs::file_type::regular", cleanup)
        self.assertIn("RetainedEntryCapability::issue(entry->path())", cleanup)
        self.assertIn("capability->removeExact()", cleanup)


if __name__ == "__main__":
    unittest.main()
