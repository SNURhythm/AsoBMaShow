"""Exercise production Activity Documents entry points with aliased storage roots."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from android_documents_mutability_tests import STUBS
from android_folder_picker_lifecycle_tests import method

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow"
ACTIVITY_STUBS = {
    "android/net/Uri.java": """
package android.net;
public class Uri {
    public final String authority, documentId;
    public Uri() { this("external.test", ""); }
    public Uri(String authority, String id) { this.authority = authority; documentId = id; }
    public String getAuthority() { return authority; }
}
""",
    "android/content/Intent.java": """
package android.content;
import android.net.Uri;
public class Intent {
    public static final String ACTION_VIEW = "view";
    public static final int FLAG_ACTIVITY_NEW_TASK = 1;
    public Uri data;
    public Intent(String action) {}
    public Intent setDataAndType(Uri value, String mime) { data = value; return this; }
    public Intent setComponent(ComponentName component) { return this; }
    public Intent addFlags(int flags) { return this; }
}
""",
    "android/content/ComponentName.java": """
package android.content;
public class ComponentName { public ComponentName(String packageName, String name) {} }
""",
    "android/content/pm/ResolveInfo.java": """
package android.content.pm;
public class ResolveInfo {
    public ActivityInfo activityInfo = new ActivityInfo();
    public static class ActivityInfo { public String packageName = "system", name = "Files"; }
}
""",
    "android/content/pm/PackageManager.java": """
package android.content.pm;
import android.content.Intent;
import java.util.List;
public class PackageManager {
    public static final int MATCH_DEFAULT_ONLY = 1, MATCH_SYSTEM_ONLY = 2;
    public List<ResolveInfo> queryIntentActivities(Intent intent, int flags) { return List.of(new ResolveInfo()); }
    public ResolveInfo resolveActivity(Intent intent, int flags) { return new ResolveInfo(); }
}
""",
    "com/snurhythm/asobmashow/SafChartFolderSource.java": """
package com.snurhythm.asobmashow;
import android.content.ContentResolver;
import android.net.Uri;
import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.util.List;
class SafChartFolderSource implements ChartFolderImport.Source {
    static final byte[] CONTENT = {1, 7, 23, 91};
    static boolean deleted;
    private final ChartFolderImport.Entry root = new ChartFolderImport.Entry("root", "root", true, -1, 1);
    private final ChartFolderImport.Entry chart = new ChartFolderImport.Entry("chart", "chart.bms", false, CONTENT.length, 1);
    SafChartFolderSource(ContentResolver resolver, Uri uri, ChartImportCopyControl control) { deleted = false; }
    public ChartFolderImport.Entry root() { return root; }
    public List<ChartFolderImport.Entry> children(ChartFolderImport.Entry entry) { return List.of(chart); }
    public InputStream open(ChartFolderImport.Entry entry) { return new ByteArrayInputStream(CONTENT); }
    public void delete(ChartFolderImport.Entry entry) { deleted = true; }
}
""",
}


class AndroidDocumentsActivityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        cls.java = str(Path(java_home) / "bin/java") if java_home else "java"
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        cls.output = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.output.cleanup)
        stubs = {**STUBS, **ACTIVITY_STUBS}
        contract = stubs["android/provider/DocumentsContract.java"]
        contract = contract.replace(
            "public static Uri buildDocumentUri(String authority, String id) { return new Uri(); }",
            "public static Uri buildDocumentUri(String authority, String id) { return new Uri(authority, id); }")
        contract = contract.replace("public static class Root {", """
    public static Uri buildRootUri(String authority, String id) { return new Uri(authority, id); }
    public static class Root {
        public static final String MIME_TYPE_ITEM = "root";
""")
        stubs["android/provider/DocumentsContract.java"] = contract
        sources = []
        for name, source in stubs.items():
            path = Path(cls.output.name) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
            sources.append(str(path))
        activity = (JAVA / "AsoBMaShowActivity.java").read_text()
        methods = "\n".join(method(activity, signature) for signature in (
            "private File documentsBmsDirectory(",
            "private ChartFolderImport.Result copyTreeUriToBmsFolder(",
            "public String openDocumentsFolder("))
        fixture = (ROOT / "tests/java/AndroidDocumentsActivityFixture.java").read_text()
        path = Path(cls.output.name) / "AndroidDocumentsActivityFixture.java"
        path.write_text(fixture.replace("// PRODUCTION_METHODS", methods))
        sources.append(str(path))
        sources += [str(JAVA / name) for name in (
            "DocumentsPathPolicy.java", "DocumentsLibraryChanges.java", "DocumentsMutationGuard.java",
            "AsoBMaShowDocumentsProvider.java", "ChartFolderImport.java", "ChartImportCopyControl.java",
            "ImportCopyWorkers.java")]
        subprocess.run([javac, "-d", cls.output.name, *sources], check=True)

    def scenario(self, name):
        subprocess.run([self.java, "-cp", self.output.name,
                        "com.snurhythm.asobmashow.AndroidDocumentsActivityFixture", name],
                       check=True, timeout=15)

    def test_internal_alias_opens_files_and_imports_copy_and_move(self):
        self.scenario("internal")

    def test_external_alias_opens_files_and_imports_copy_and_move(self):
        self.scenario("external")

    def test_symlinked_bms_directory_remains_rejected(self):
        self.scenario("symlink")


if __name__ == "__main__":
    unittest.main()
