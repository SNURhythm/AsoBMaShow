"""Run the activity's native music lifecycle against headless Android boundaries."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class AndroidNativeMusicFocusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(
                ["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        cls.java = str(Path(java_home) / "bin/java") if java_home else "java"
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        source = (ROOT / "android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java").read_text()
        signatures = [
            "private boolean hasNativeMusicLocked()",
            "private boolean isNativeMusicPlayingLocked()",
            "private boolean requestNativeMusicFocusLocked()",
            "private void abandonNativeMusicFocusLocked()",
            "public String loadNativeMusicStream(",
            "public String playNativeMusic()",
            "public String pauseNativeMusic()",
            "public String stopNativeMusic()",
            "private void ensureNativeMusicSessionLocked()",
            "private void updateNativeMusicForegroundServiceLocked(boolean playing)",
            "private void releaseNativeMusicPlayerLocked()",
            "private void releaseNativeMusicPlayerLocked(boolean preserveSession)",
        ]
        # Include the internal pause overload when present; the public method
        # remains the entrypoint in both the pre-fix and fixed code.
        if "private String pauseNativeMusicLocked(" in source:
            signatures.append("private String pauseNativeMusicLocked(")
        for signature in ["private void startNativeMusicTransport("]:
            if signature in source:
                signatures.append(signature)
        service = (ROOT / "android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowMusicService.java").read_text()
        service_methods = ["public int onStartCommand(", "private void readUpdate(",
                           "private String nonEmpty(", "private void sendTransportAction(",
                           "private void updateNotification()", "private PendingIntent serviceIntent("]
        fixture = (ROOT / "tests/java/AndroidNativeMusicFocusFixture.java").read_text()
        fixture = fixture.replace("// ACTIVITY_METHODS", "\n".join(method(source, s) for s in signatures))
        constants = service[service.index("    public static final String ACTION_UPDATE"):service.index("    private static final String CHANNEL_ID")]
        fixture = fixture.replace("// SERVICE_METHODS", constants + "\n".join(method(service, s) for s in service_methods))
        cls.output = tempfile.TemporaryDirectory()
        generated = Path(cls.output.name) / "AndroidNativeMusicFocusFixture.java"
        generated.write_text(fixture)
        subprocess.run([javac, "-d", cls.output.name, str(generated)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.output.cleanup()

    def run_case(self, name):
        result = subprocess.run([self.java, "-ea", "-cp", self.output.name,
                                 "AndroidNativeMusicFocusFixture", name],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_latest_queued_remote_request_keeps_foreground_eligibility(self):
        self.run_case("remote-latest")

    def test_deferred_remote_play_cannot_override_pause_stop_or_replacement(self):
        self.run_case("remote-cancel")

    def test_remote_play_restores_foreground_before_focus(self):
        self.run_case("remote-play")

    def test_remote_skips_restore_foreground_through_queue_preparation(self):
        self.run_case("remote-skip")

    def test_remote_failures_release_bootstrap_foreground(self):
        self.run_case("remote-failure")

    def test_notification_actions_cannot_override_later_transport_changes(self):
        self.run_case("notification-cancel")

    def test_notification_play_does_not_recurse_through_session_play(self):
        self.run_case("notification-play")

    def test_start_failure_releases_focus(self):
        self.run_case("start-failure")

    def test_explicit_pause_releases_focus_and_invalidates_callbacks(self):
        self.run_case("pause")

    def test_transient_loss_resumes_but_permanent_loss_does_not(self):
        self.run_case("interruption")

    def test_metadata_failure_after_start_preserves_focus(self):
        self.run_case("metadata-failure")

    def test_stream_failure_unloads_session_and_ignores_stale_callbacks(self):
        self.run_case("stream-failure")


class AndroidMusicStreamConstructionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(
                ["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        cls.java = str(Path(java_home) / "bin/java") if java_home else "java"
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        java_root = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow"
        stream = (java_root / "AndroidMusicStream.java").read_text()
        player = (java_root / "NativePcmMusicPlayer.java").read_text()
        stream = "\n".join(line for line in stream.splitlines()
                           if not line.startswith(("package ", "import ")))
        player = "\n".join(line for line in player.splitlines()
                           if not line.startswith("package "))
        # Substitute only JNI declarations; construction and cleanup run from source.
        native_calls = {
            "int read(long handle, long firstFrame, short[] samples)": "return NativeBoundary.read(handle, firstFrame, samples);",
            "long duration(long handle)": "return NativeBoundary.duration(handle);",
            "String error(long handle)": "return NativeBoundary.error(handle);",
            "void enable(long handle, boolean value)": "NativeBoundary.enable(handle, value);",
            "void seek(long handle, long firstFrame)": "NativeBoundary.seek(handle, firstFrame);",
            "void release(long handle)": "NativeBoundary.release(handle);",
            "void reportError(String message)": "NativeBoundary.reportError(message);",
        }
        for signature, body in native_calls.items():
            stream = stream.replace("static native " + signature + ";",
                                    "static " + signature + " { " + body + " }")
        stream = stream.replace("final class AndroidMusicStream", "static final class AndroidMusicStream", 1)
        player = player.replace("final class NativePcmMusicPlayer", "static final class NativePcmMusicPlayer", 1)
        fixture = (ROOT / "tests/java/AndroidMusicStreamFixture.java").read_text()
        fixture = fixture.replace("// STREAM_IMPLEMENTATION", stream).replace("// PLAYER_IMPLEMENTATION", player)
        cls.output = tempfile.TemporaryDirectory()
        generated = Path(cls.output.name) / "AndroidMusicStreamFixture.java"
        generated.write_text(fixture)
        subprocess.run([javac, "-d", cls.output.name, str(generated)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.output.cleanup()

    def run_case(self, *args):
        result = subprocess.run([self.java, "-ea", "-cp", self.output.name,
                                 "AndroidMusicStreamFixture", *args],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_construction_failures_release_native_source_and_any_created_track(self):
        for boundary in ("minimum", "unavailable", "build", "state", "uninitialized",
                         "duration", "enable", "start"):
            with self.subTest(boundary=boundary):
                self.run_case(boundary)

    def test_allocation_errors_also_release_both_resources(self):
        for boundary in ("build", "state", "duration", "enable", "start"):
            with self.subTest(boundary=boundary):
                self.run_case(boundary, "normal", "error")

    def test_cleanup_failures_preserve_original_and_do_not_skip_other_resource(self):
        for boundary in ("minimum", "state", "uninitialized", "duration", "enable", "start"):
            with self.subTest(boundary=boundary):
                self.run_case(boundary, "cleanup")

    def test_success_transfers_ownership_to_feeder_and_closes_exactly_once(self):
        self.run_case("success")


if __name__ == "__main__":
    unittest.main()
