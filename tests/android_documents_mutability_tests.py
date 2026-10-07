"""Run the full DocumentsProvider against host filesystem/Android boundary doubles."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow"

STUBS = {
    "android/content/Context.java": """
package android.content;
import java.io.File;
public class Context {
    public static final int MODE_PRIVATE = 0;
    private final File root;
    private final java.util.Map<String, SharedPreferences> preferences = new java.util.HashMap<>();
    public Context(File root) { this.root = root; }
    public File getExternalFilesDir(String type) { return root; }
    public File getFilesDir() { return root; }
    public Context getApplicationContext() { return this; }
    public String getString(int id) { return "AsoBMaShow"; }
    public ContentResolver getContentResolver() { return new ContentResolver(); }
    public SharedPreferences getSharedPreferences(String name, int mode) {
        return preferences.computeIfAbsent(name, ignored -> new SharedPreferences());
    }
}
""",
    "android/content/ContentResolver.java": """
package android.content;
import android.net.Uri;
public class ContentResolver { public void notifyChange(Uri uri, Object observer) {} }
""",
    "android/content/SharedPreferences.java": """
package android.content;
public class SharedPreferences {
    private final java.util.Map<String, Boolean> values = new java.util.HashMap<>();
    public boolean getBoolean(String key, boolean fallback) { return values.getOrDefault(key, fallback); }
    public Editor edit() { return new Editor(); }
    public class Editor {
        private final java.util.Map<String, Boolean> pending = new java.util.HashMap<>();
        public Editor putBoolean(String key, boolean value) { pending.put(key, value); return this; }
        public void apply() { values.putAll(pending); }
    }
}
""",
    "android/database/Cursor.java": """
package android.database;
public interface Cursor {}
""",
    "android/database/MatrixCursor.java": """
package android.database;
import java.util.*;
import android.net.Uri;
import android.content.ContentResolver;
public class MatrixCursor implements Cursor {
    public final List<Map<String, Object>> rows = new ArrayList<>();
    public MatrixCursor(String[] columns) {}
    public RowBuilder newRow() {
        Map<String, Object> row = new HashMap<>(); rows.add(row); return new RowBuilder(row);
    }
    public void setNotificationUri(ContentResolver resolver, Uri uri) {}
    public static class RowBuilder {
        private final Map<String, Object> row;
        RowBuilder(Map<String, Object> row) { this.row = row; }
        public RowBuilder add(String column, Object value) { row.put(column, value); return this; }
    }
}
""",
    "android/net/Uri.java": """
package android.net;
public class Uri {}
""",
    "android/os/CancellationSignal.java": """
package android.os;
public class CancellationSignal { public void throwIfCanceled() {} }
""",
    "android/os/Looper.java": """
package android.os;
public class Looper { public static Looper getMainLooper() { return new Looper(); } }
""",
    "android/os/Handler.java": """
package android.os;
public class Handler { public Handler(Looper looper) {} }
""",
    "android/os/SystemClock.java": """
package android.os;
public class SystemClock { public static long elapsedRealtime() { return System.nanoTime()/1000000; } }
""",
    "android/os/ParcelFileDescriptor.java": """
package android.os;
import java.io.*;
public class ParcelFileDescriptor implements AutoCloseable {
    public static final int MODE_READ_ONLY = 0x10000000, MODE_WRITE_ONLY = 0x20000000;
    public static final int MODE_READ_WRITE = 0x30000000, MODE_CREATE = 0x08000000;
    public static final int MODE_TRUNCATE = 0x04000000, MODE_APPEND = 0x02000000;
    public static int opened;
    private final RandomAccessFile file;
    private final OnCloseListener close;
    public interface OnCloseListener { void onClose(IOException error); }
    private ParcelFileDescriptor(File path, int mode, OnCloseListener close) throws FileNotFoundException {
        ++opened;
        this.close = close;
        file = new RandomAccessFile(path, (mode & MODE_WRITE_ONLY) != 0 ? "rw" : "r");
        try {
            if ((mode & MODE_TRUNCATE) != 0) file.setLength(0);
            if ((mode & MODE_APPEND) != 0) file.seek(file.length());
        } catch (IOException error) { throw new FileNotFoundException(error.toString()); }
    }
    public static int parseMode(String mode) {
        switch (mode) {
        case "r": return MODE_READ_ONLY;
        case "w": case "wt": return MODE_WRITE_ONLY | MODE_CREATE | MODE_TRUNCATE;
        case "wa": return MODE_WRITE_ONLY | MODE_CREATE | MODE_APPEND;
        case "rw": return MODE_READ_WRITE | MODE_CREATE;
        case "rwt": return MODE_READ_WRITE | MODE_CREATE | MODE_TRUNCATE;
        default: throw new IllegalArgumentException("Invalid mode");
        }
    }
    public static ParcelFileDescriptor open(File file, int mode) throws FileNotFoundException {
        return new ParcelFileDescriptor(file, mode, null);
    }
    public static ParcelFileDescriptor open(File file, int mode, Handler handler, OnCloseListener close)
            throws IOException { return new ParcelFileDescriptor(file, mode, close); }
    public byte[] readBytes() throws IOException {
        byte[] bytes = new byte[(int)file.length()]; file.readFully(bytes); return bytes;
    }
    public void writeBytes(byte[] bytes) throws IOException { file.write(bytes); }
    public void close() throws IOException { file.close(); if (close != null) close.onClose(null); }
}
""",
    "android/provider/DocumentsContract.java": """
package android.provider;
import java.util.List;
import android.net.Uri;
public class DocumentsContract {
    public static Uri buildRootsUri(String authority) { return new Uri(); }
    public static Uri buildDocumentUri(String authority, String id) { return new Uri(); }
    public static Uri buildChildDocumentsUri(String authority, String id) { return new Uri(); }
    public static class Path { public Path(String root, List<String> ids) {} }
    public static class Root {
        public static final String COLUMN_ROOT_ID="root", COLUMN_DOCUMENT_ID="document", COLUMN_TITLE="title",
                COLUMN_FLAGS="flags", COLUMN_ICON="icon", COLUMN_AVAILABLE_BYTES="bytes", COLUMN_MIME_TYPES="mimes";
        public static final int FLAG_SUPPORTS_CREATE=1, FLAG_SUPPORTS_IS_CHILD=2, FLAG_LOCAL_ONLY=4;
    }
    public static class Document {
        public static final String COLUMN_DOCUMENT_ID="id", COLUMN_DISPLAY_NAME="name", COLUMN_MIME_TYPE="mime",
                COLUMN_FLAGS="flags", COLUMN_SIZE="size", COLUMN_LAST_MODIFIED="modified";
        public static final String MIME_TYPE_DIR="vnd.android.document/directory";
        public static final int FLAG_SUPPORTS_WRITE=2, FLAG_SUPPORTS_DELETE=4, FLAG_DIR_SUPPORTS_CREATE=8,
                FLAG_SUPPORTS_RENAME=64;
    }
}
""",
    "android/provider/DocumentsProvider.java": """
package android.provider;
import android.content.Context;
import android.database.Cursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import java.io.FileNotFoundException;
public abstract class DocumentsProvider {
    private Context context;
    public void attachContext(Context context) { this.context = context; }
    public Context getContext() { return context; }
    public abstract boolean onCreate();
    public abstract Cursor queryRoots(String[] projection);
    public abstract Cursor queryDocument(String id, String[] projection) throws FileNotFoundException;
    public abstract Cursor queryChildDocuments(String id, String[] projection, String order) throws FileNotFoundException;
    public abstract boolean isChildDocument(String parent, String child);
    public abstract DocumentsContract.Path findDocumentPath(String parent, String child) throws FileNotFoundException;
    public abstract ParcelFileDescriptor openDocument(String id, String mode, CancellationSignal signal) throws FileNotFoundException;
    public abstract String createDocument(String parent, String mime, String name) throws FileNotFoundException;
    public abstract String renameDocument(String id, String name) throws FileNotFoundException;
    public abstract void deleteDocument(String id) throws FileNotFoundException;
}
""",
    "android/webkit/MimeTypeMap.java": """
package android.webkit;
public class MimeTypeMap {
    public static MimeTypeMap getSingleton() { return new MimeTypeMap(); }
    public String getMimeTypeFromExtension(String extension) { return null; }
}
""",
    "com/snurhythm/asobmashow/AppResources.java": """
package com.snurhythm.asobmashow;
class BuildConfig { static final String APPLICATION_ID="com.snurhythm.asobmashow"; }
class R { static class string { static final int app_name=1; } static class mipmap { static final int ic_launcher=2; } }
""",
}


class AndroidDocumentsMutabilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        cls.java = str(Path(java_home) / "bin/java") if java_home else "java"
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        cls.output = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.output.cleanup)
        sources = []
        for name, source in STUBS.items():
            path = Path(cls.output.name) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
            sources.append(str(path))
        sources += [str(JAVA / name) for name in (
            "DocumentsPathPolicy.java", "DocumentsLibraryChanges.java", "DocumentsMutationGuard.java",
            "AsoBMaShowDocumentsProvider.java", "ChartFolderImport.java", "ChartImportCopyControl.java",
            "ImportCopyWorkers.java")]
        sources.append(str(ROOT / "tests/java/DocumentsMutabilityTests.java"))
        subprocess.run([javac, "-d", cls.output.name, *sources], check=True)

    def scenario(self, name):
        subprocess.run([self.java, "-cp", self.output.name,
                        "com.snurhythm.asobmashow.DocumentsMutabilityTests", name], check=True, timeout=15)

    def test_import_reservation_blocks_all_provider_mutations(self):
        self.scenario("reserved-import")

    def test_move_keeps_completed_children_protected_until_root_finishes(self):
        self.scenario("reserved-move")

    def test_write_modes_rejected_before_open_or_truncate(self):
        for mode in ("w", "wt", "wa", "rw", "rwt"):
            with self.subTest(mode=mode):
                self.scenario("write-" + mode)

    def test_read_only_flags_and_read_access(self):
        self.scenario("read-and-flags")

    def test_protected_ancestors_cannot_be_renamed_or_deleted(self):
        self.scenario("ancestors")

    def test_reserved_root_names_cannot_be_created(self):
        self.scenario("create-reserved")

    def test_reserved_root_names_cannot_be_rename_destinations(self):
        self.scenario("rename-reserved")

    def test_protected_tree_rejects_creation(self):
        self.scenario("create-inside")

    def test_ordinary_documents_remain_mutable(self):
        self.scenario("ordinary")

    def test_writers_remain_tracked_after_folder_is_renamed_into_bms(self):
        self.scenario("renamed-writers")

    def test_internal_storage_fallback_accepts_aliased_ancestors(self):
        self.scenario("fallback-alias")

    def test_bms_case_alias_mutations_persist_library_refresh(self):
        for alias in ("BMS", "bms", "BmS"):
            with self.subTest(alias=alias):
                self.scenario("refresh-" + alias)

    def test_path_aliases_cannot_bypass_protection(self):
        self.scenario("aliases")


if __name__ == "__main__":
    unittest.main()
