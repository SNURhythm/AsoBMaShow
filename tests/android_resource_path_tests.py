"""Exercise the Activity's production SAF lookup against a case-sensitive provider."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from android_folder_picker_lifecycle_tests import method

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow"


class AndroidResourcePathTests(unittest.TestCase):
    def test_resource_lookup(self):
        home = os.environ.get("JAVA_HOME")
        if not home and Path("/usr/libexec/java_home").exists():
            home = subprocess.check_output(["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        javac = str(Path(home) / "bin/javac") if home else "javac"
        java = str(Path(home) / "bin/java") if home else "java"
        activity = (JAVA / "AsoBMaShowActivity.java").read_text()
        signatures = ["private String resolveDocumentId(", "private String findChildDocumentId(",
                      "private int cacheDirectChildren(", "public String clearTransientTreeFileCache(",
                      "private String normalizeRelativePath(", "public String listTreeTextFiles(",
                      "public String cacheTreeDirectory("]
        for optional in ["private ResourceDirectoryCache.Directory readResourceDirectory("]:
            if optional in activity:
                signatures.append(optional)
        methods = "\n".join(method(activity, signature) for signature in signatures).replace("org.json.JSONArray", "JsonNames")
        fixture = (ROOT / "tests/java/AndroidResourcePathFixture.java").read_text()
        helper = JAVA / "ResourceDirectoryCache.java"
        fields = "private final ResourceDirectoryCache resourceDirectoryCache = new ResourceDirectoryCache();" if helper.exists() else ""
        with tempfile.TemporaryDirectory() as output:
            generated = Path(output) / "AndroidResourcePathFixture.java"
            generated.write_text(fixture.replace("// PRODUCTION_METHODS", methods).replace("// CACHE_FIELD", fields))
            subprocess.run([javac, "-d", output, str(generated), *([str(helper)] if helper.exists() else [])], check=True)
            subprocess.run([java, "-cp", output, "com.snurhythm.asobmashow.AndroidResourcePathFixture"], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
