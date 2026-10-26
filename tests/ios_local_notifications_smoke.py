#!/usr/bin/env python3
"""Optional real UserNotifications check on an already booted iOS simulator.

Run with --simulator <UDID>. Installs a temporary test-only app, grants it
provisional notification permission, checks replacement/foreground cancellation,
and removes it. Does not install or launch the game or test OS delivery/taps.
"""
import argparse
import pathlib
import plistlib
import subprocess
import tempfile
import time


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", required=True)
    simulator = parser.parse_args().simulator
    root = pathlib.Path(__file__).resolve().parents[1]
    bundle = "com.snurhythm.combo-notification-smoke"
    with tempfile.TemporaryDirectory(prefix="combo-notification-smoke-") as directory:
        app = pathlib.Path(directory) / "ComboSmoke.app"
        app.mkdir()
        info = {
            "CFBundleIdentifier": bundle, "CFBundleName": "ComboSmoke",
            "CFBundleExecutable": "ComboSmoke", "CFBundlePackageType": "APPL",
            "CFBundleVersion": "1", "CFBundleShortVersionString": "1.0",
            "MinimumOSVersion": "15.0", "UIDeviceFamily": [1, 2], "UILaunchScreen": {},
            "UIApplicationSceneManifest": {
                "UIApplicationSupportsMultipleScenes": False,
                "UISceneConfigurations": {"UIWindowSceneSessionRoleApplication": [{
                    "UISceneConfigurationName": "Smoke", "UISceneDelegateClassName": "SmokeScene"}]}}}
        (app / "Info.plist").write_bytes(plistlib.dumps(info))
        sdk = run("xcrun", "--sdk", "iphonesimulator", "--show-sdk-path", capture_output=True).stdout.strip()
        run("xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++23", "-fobjc-arc",
            "-target", "arm64-apple-ios15.0-simulator", "-isysroot", sdk,
            "-I" + str(root / "SDL/include"), "-I" + str(root / "src"),
            str(root / "tests/ios_local_notifications_smoke.mm"),
            str(root / "src/notifications/LocalNotificationsIOS.mm"),
            "-framework", "UIKit", "-framework", "Foundation", "-framework", "UserNotifications",
            "-o", str(app / "ComboSmoke"))
        run("codesign", "--force", "--sign", "-", str(app), capture_output=True)
        run("xcrun", "simctl", "install", simulator, str(app))
        try:
            data = pathlib.Path(run("xcrun", "simctl", "get_app_container", simulator, bundle, "data",
                                    capture_output=True).stdout.strip())
            status = data / "Documents/status.txt"
            status.unlink(missing_ok=True)
            run("xcrun", "simctl", "launch", "--terminate-running-process", simulator, bundle)
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                text = status.read_text() if status.exists() else ""
                if text.startswith("PASS:"):
                    print(text)
                    return
                if text.startswith("FAIL:"):
                    raise RuntimeError(text)
                time.sleep(0.1)
            raise RuntimeError("Timed out waiting for the notification adapter: " + text)
        finally:
            subprocess.run(["xcrun", "simctl", "terminate", simulator, bundle],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            run("xcrun", "simctl", "uninstall", simulator, bundle)


if __name__ == "__main__":
    main()
