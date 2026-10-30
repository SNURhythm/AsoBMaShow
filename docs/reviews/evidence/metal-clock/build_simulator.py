#!/usr/bin/env python3
"""Build standalone research harnesses; never installs or distributes an app."""
import argparse
import pathlib
import plistlib
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sdl-framework", required=True, type=pathlib.Path,
                    help="existing arm64 iOS Simulator SDL3.framework")
parser.add_argument("--output", required=True, type=pathlib.Path)
args = parser.parse_args()
source = pathlib.Path(__file__).resolve().parent
root = source.parents[3]
sdk = subprocess.check_output(
    ["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"], text=True).strip()
for executable, bundle_id, scene, harness in [
    ("ClockAudit", "com.snurhythm.sdl-clock-audit", "SDLUIKitSceneDelegate", "sdl_clock_harness.mm"),
    ("Audit", "com.snurhythm.metal-transaction-audit", "AuditScene", "transaction_harness.mm"),
]:
    app = args.output / (executable + ".app")
    app.mkdir(parents=True, exist_ok=True)
    info = {
        "CFBundleExecutable": executable, "CFBundleIdentifier": bundle_id,
        "CFBundleName": executable, "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0", "CFBundleVersion": "1",
        "MinimumOSVersion": "17.0", "UIDeviceFamily": [1, 2], "UILaunchScreen": {},
        "UIApplicationSupportsIndirectInputEvents": True,
        "CADisableMinimumFrameDurationOnPhone": True,
        "UISupportedInterfaceOrientations": ["UIInterfaceOrientationPortrait",
            "UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"],
        "UIApplicationSceneManifest": {
            "UIApplicationSupportsMultipleScenes": False,
            "UISceneConfigurations": {"UIWindowSceneSessionRoleApplication": [
                {"UISceneConfigurationName": "Audit", "UISceneDelegateClassName": scene}]},
        },
    }
    (app / "Info.plist").write_bytes(plistlib.dumps(info))
    command = ["xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++23", "-fobjc-arc",
        "-target", "arm64-apple-ios17.0-simulator", "-isysroot", sdk,
        "-I" + str(root / "src"), "-I" + str(root / "SDL/include"), str(source / harness),
        "-framework", "Foundation", "-framework", "UIKit", "-framework", "QuartzCore",
        "-framework", "Metal", "-o", str(app / executable)]
    if executable == "ClockAudit":
        frameworks = app / "Frameworks"
        shutil.copytree(args.sdl_framework, frameworks / "SDL3.framework", dirs_exist_ok=True)
        command += ["-F" + str(frameworks), "-framework", "SDL3", "-Wl,-rpath,@executable_path/Frameworks"]
    subprocess.run(command, check=True)
    print(app)
