from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from support.fixture_compiler import FixtureCompiler


ROOT = Path(__file__).resolve().parents[1]


class AndroidLocaleTests(unittest.TestCase):
    def test_full_tags_take_priority_and_bridge_failure_uses_sdl(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "SDL2").mkdir()
            (output / "SDL2/SDL.h").write_text("""
#pragma once
struct SDL_Locale { const char *language; const char *country; };
SDL_Locale *SDL_GetPreferredLocales();
void SDL_free(void *);
""")
            source = output / "locale_test.cpp"
            source.write_text(r'''
#define TARGET_OS_ANDROID 1
#include "i18n/PlatformLocale.h"
#include <cassert>
std::string androidTags;
int androidCalls = 0;
int sdlCalls = 0;
std::string GetAndroidPreferredLanguageTags() { ++androidCalls; return androidTags; }
SDL_Locale *SDL_GetPreferredLocales() {
  ++sdlCalls;
  static SDL_Locale locales[] = {{"zh", "TW"}, {nullptr, nullptr}};
  return locales;
}
void SDL_free(void *) {}
namespace i18n {
Language current = Language::English;
void setLanguage(Language value) { current = value; }
Language language() { return current; }
}
int main() {
  using namespace i18n;
  androidTags = "fr-FR,zh-Hans-TW,en-US";
  initializePlatformLanguage("system");
  assert(language() == Language::SimplifiedChinese);
  assert(sdlCalls == 0);
  androidTags = "zh-Hant-CN,en-US";
  initializePlatformLanguage("system");
  assert(language() == Language::TraditionalChinese);
  androidTags = "en-US,zh-Hant-CN";
  initializePlatformLanguage("system");
  assert(language() == Language::English);
  androidTags = "fr-FR";
  initializePlatformLanguage("system");
  assert(language() == Language::English);
  assert(sdlCalls == 0);
  androidTags = "zh-Hant-CN";
  const int before = androidCalls;
  initializePlatformLanguage("zh-Hans");
  assert(language() == Language::SimplifiedChinese);
  assert(androidCalls == before);
  androidTags.clear();
  initializePlatformLanguage("system");
  assert(language() == Language::TraditionalChinese);
  assert(sdlCalls == 1);
}
''')
            compiler = FixtureCompiler.from_environment()
            binary = output / ("locale_test" + compiler.executable_suffix)
            compiler.build([source], binary, output, includes=[output, ROOT / "src"])
            subprocess.run([str(binary)], check=True)

    def test_activity_preserves_scripts_and_preference_order(self):
        activity = (ROOT / "android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java").read_text()
        signature = "public String getPreferredLanguageTags()"
        start = activity.index(signature)
        opening = activity.index("{", start)
        depth, end = 1, opening + 1
        while depth:
            depth += (activity[end] == "{") - (activity[end] == "}")
            end += 1
        method = activity[start:end]
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            discovered = subprocess.run(["/usr/libexec/java_home", "-v", "17"],
                                        text=True, capture_output=True)
            if discovered.returncode == 0:
                java_home = discovered.stdout.strip()
        java = shutil.which(str(Path(java_home) / "bin/java") if java_home else "java")
        javac = shutil.which(str(Path(java_home) / "bin/javac") if java_home else "javac")
        if not java or not javac:
            self.skipTest("Supplementary Java locale smoke test requires a JDK")
        # macOS may provide launcher stubs even when no JDK is installed.
        for executable in (java, javac):
            probe = subprocess.run([executable, "-version"], capture_output=True)
            if probe.returncode != 0:
                self.skipTest("Supplementary Java locale smoke test requires a working JDK")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "LocaleActivityFixture.java"
            source.write_text('''
import java.util.Locale;
public class LocaleActivityFixture {
    static class LocaleList {
        public String toLanguageTags() {
            return Locale.forLanguageTag("zh-Hant-CN").toLanguageTag() + "," +
                   Locale.forLanguageTag("zh-Hans-TW").toLanguageTag();
        }
    }
    static class Configuration {
        public LocaleList getLocales() { return new LocaleList(); }
    }
    static class Resources {
        public Configuration getConfiguration() { return new Configuration(); }
    }
    public Resources getResources() { return new Resources(); }
    ''' + method + '''
    public static void main(String[] args) {
        String tags = new LocaleActivityFixture().getPreferredLanguageTags();
        if (!tags.equals("zh-Hant-CN,zh-Hans-TW")) throw new AssertionError(tags);
    }
}
''')
            subprocess.run([javac, "-d", directory, str(source)], check=True)
            subprocess.run([java, "-cp", directory, "LocaleActivityFixture"], check=True)


if __name__ == "__main__":
    unittest.main()
