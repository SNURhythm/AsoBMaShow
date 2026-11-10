from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from find_bms_transport_extract import function

ROOT = Path(__file__).resolve().parents[1]

class BackgroundDownloadTests(unittest.TestCase):
    def test_notification_tap_native_bridge(self):
        java_home = os.environ.get('JAVA_HOME')
        if not java_home and sys.platform == 'darwin':
            java_home = subprocess.check_output(['/usr/libexec/java_home', '-v', '17'], text=True).strip()
        javac = str(Path(java_home) / 'bin/javac') if java_home else shutil.which('javac')
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not javac or not compiler or sys.platform not in ('darwin', 'linux'):
            self.skipTest('JDK and native compiler required for JNI fixture')
        java_home = Path(java_home) if java_home else Path(javac).resolve().parent.parent
        native = (ROOT / 'src/downloads/DownloadPlatformAndroid.cpp').read_text()
        service = (ROOT / 'src/downloads/DownloadService.cpp').read_text()
        java_source = (ROOT / 'android/app/src/main/java/com/snurhythm/asobmashow/BmsDownloads.java').read_text()
        callback = 'Java_com_snurhythm_asobmashow_BmsDownloads_nativeShowDownloads('
        bridge = ('extern "C" JNIEXPORT void JNICALL\n' + function(native, callback)) if callback in native else ''
        with tempfile.TemporaryDirectory() as output:
            output = Path(output)
            source = output / 'bridge.cpp'
            source.write_text('''#include <jni.h>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
namespace downloads {
std::mutex presentationMutex;
std::optional<std::string> requestedPresentation;
void requestDownloadsPresentation(const std::string &id = {});
''' + function(service, 'void requestDownloadsPresentation(') + '\n' +
                function(service, 'std::optional<std::string> takeDownloadsPresentation(') + '\n}\n' +
                function(native, 'void bridgeFailure(') + '\n' + bridge + '''
extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_BmsDownloads_take(JNIEnv *, jclass) {
  return downloads::takeDownloadsPresentation().has_value();
}
''')
            fixture = output / 'BmsDownloads.java'
            fixture.write_text('''package com.snurhythm.asobmashow;
public class BmsDownloads {
  static class Intent {
    boolean downloads;
    Intent(boolean value) { downloads = value; }
    boolean getBooleanExtra(String name, boolean fallback) { return downloads; }
    void removeExtra(String name) { downloads = false; }
  }
  private static native void nativeShowDownloads();
  private static native boolean take();
''' + function(java_source, 'static void acceptTap(') + '''
  public static void main(String[] args) {
    System.load(args[0]);
    acceptTap(null); acceptTap(new Intent(false));
    if (take()) throw new AssertionError("unrelated launch opened Downloads");
    Intent intent = new Intent(true);
    acceptTap(intent);
    if (!take() || take()) throw new AssertionError("tap must queue one presentation before service initialization");
    acceptTap(intent);
    if (take()) throw new AssertionError("consumed intent must not reopen Downloads");
  }
}
''')
            library = output / ('libtap.dylib' if sys.platform == 'darwin' else 'libtap.so')
            subprocess.run([compiler, '-std=c++20', '-shared', '-fPIC', '-pthread',
                '-I', str(java_home / 'include'), '-I', str(java_home / 'include' / sys.platform),
                str(source), '-o', str(library)], check=True)
            subprocess.run([javac, '-d', str(output), str(fixture)], check=True)
            subprocess.run([str(java_home / 'bin/java'), '-Xcheck:jni', '-cp', str(output),
                'com.snurhythm.asobmashow.BmsDownloads', str(library)], check=True, timeout=10)

    def test_transport(self):
        java_home = os.environ.get('JAVA_HOME')
        if not java_home and sys.platform == 'darwin':
            java_home = subprocess.check_output(['/usr/libexec/java_home', '-v', '17'], text=True).strip()
        javac = str(Path(java_home) / 'bin/javac') if java_home else shutil.which('javac')
        java = str(Path(java_home) / 'bin/java') if java_home else shutil.which('java')
        if not javac or not java:
            self.skipTest('JDK unavailable')
        with tempfile.TemporaryDirectory() as output:
            subprocess.run([javac, '-d', output,
                str(ROOT / 'android/app/src/main/java/com/snurhythm/asobmashow/BmsDownloadTransport.java'),
                str(ROOT / 'tests/java/BmsDownloadTransportTest.java')], check=True)
            subprocess.run([java, '-cp', output, 'com.snurhythm.asobmashow.BmsDownloadTransportTest', output], check=True, timeout=20)

if __name__ == '__main__': unittest.main()
