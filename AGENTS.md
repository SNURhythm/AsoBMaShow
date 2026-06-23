# Repository Notes for Agents

## iOS Firebase Deploy

- To deploy an iOS build to Firebase App Distribution from this machine, use:
  `scripts/ios_firebase_deploy.sh`
- Do not run `bundle exec fastlane ios beta` directly unless you have already selected the project Ruby. The default Ruby in this environment is the macOS system Ruby, which is too old for this repo.
- The deploy script loads `.env`, `.env.local`, `ios/Xcode/AsoBMaShow/.env`, and `ios/Xcode/AsoBMaShow/.env.local`, then selects the Ruby version from `ios/Xcode/AsoBMaShow/.ruby-version` or `.tool-versions`.
- Use `scripts/ios_firebase_deploy.env.example` as the private env template. Real env files must stay out of git.
- The script simulates the GitHub PR-to-`develop` environment so the Fastlane `ios beta` lane takes the Firebase App Distribution path instead of TestFlight.
- For a fast local compile check without archive, signing, or Firebase upload, run:
  `scripts/ios_firebase_deploy.sh --build-only`
  This still selects the project Ruby and runs `scripts/ios_init.sh` unless `--skip-init` is passed.
- Running the deploy script uploads a build. Only run it when the user explicitly asks for deployment.

## iOS Build Setup

- `scripts/ios_init.sh` configures the generated bgfx Xcode project and caches only Bundler gems and CocoaPods. It intentionally does not cache `bgfx/build`.
- TestFlight clean builds are intentional. Do not remove the TestFlight clean behavior without the user asking.
- Firebase PR builds should keep the fixed DerivedData path so Xcode can reuse `ArchiveIntermediates`.
- When adding a new source file under `src`, add its path to `membershipExceptions` in `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj` so the app target builds it.

## Android Firebase Deploy

- To deploy an Android build to Firebase App Distribution from this machine, use:
  `scripts/android_firebase_deploy.sh`
- For a fast local compile check without upload, run:
  `scripts/android_firebase_deploy.sh --build-only`
- The Android deploy script loads `.env`, `.env.local`, `android/.env`, and `android/.env.local`.
- Use `scripts/android_firebase_deploy.env.example` as the private env template. Real env files must stay out of git.
- The script can infer `FIREBASE_ANDROID_APP_ID` and `FIREBASE_PROJECT` from `android/app/google-services.json`.
- Leave `ANDROID_VERSION_CODE` empty unless the user explicitly wants an override. Build-only and deploy runs both use an automatic compact UTC timestamp version code; the script does not query Firebase releases for versioning.
- Running the deploy script uploads a build. Only run it without `--build-only` when the user explicitly asks for deployment.
- GitHub Actions deploys Android from `.github/workflows/ios-testflight.yml` only for commits pushed to `develop`, and only after the iOS/TestFlight job succeeds. Keep iOS first unless the user asks to change release ordering.

## Android Emulator Testing

- The Homebrew Android SDK root on this machine is:
  `/opt/homebrew/share/android-commandlinetools`
- For headless smoke tests, prefer the `android10` AVD. `Pixel_7a_API_33_GooglePlay` has been observed to boot as `RUNNING_LOCKED` in headless mode and can reject app launches with a misleading `Activity class ... does not exist` error.
- Build first:
  `scripts/android_firebase_deploy.sh --build-only`
- Boot the emulator:
  `/opt/homebrew/share/android-commandlinetools/emulator/emulator -avd android10 -no-window -gpu swiftshader_indirect -no-snapshot -no-audio -no-boot-anim`
- Wait for boot and confirm the user is unlocked:
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb wait-for-device`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell getprop sys.boot_completed`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell dumpsys user | sed -n '1,35p'`
- Install and launch with the explicit component. `monkey -p` may fail to resolve the launcher in headless emulator tests:
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb install -r android/app/build/outputs/apk/debug/app-debug.apk`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb logcat -c`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell am start --user 0 -n com.snurhythm.asobmashow/.AsoBMaShowActivity`
- Useful runtime checks:
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell pidof com.snurhythm.asobmashow`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell dumpsys activity activities | rg -n "ResumedActivity|com.snurhythm|documentsui|AsoBMaShow"`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell logcat -d -v time | rg -i "AsoBMaShow|SDL|bgfx|AndroidRuntime|FATAL EXCEPTION|Fatal signal|tombstone|ANR|renderer|Vulkan|OpenGLES"`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb exec-out screencap -p > /tmp/asobmashow-android.png`
- On first launch with an empty library, the app intentionally opens Android's `ACTION_OPEN_DOCUMENT_TREE` picker. In logs, a good graphics startup includes `bgfx renderer: Vulkan`; the current foreground UI may be `com.android.documentsui/.picker.PickActivity` until a folder is granted.
- Stop the emulator when done:
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb emu kill`

## BMS Parser Updates

- `src/bms_parser.hpp` and `src/bms_parser.cpp` are amalgamated from `../bms-parser-cpp`. Do not edit parser code directly in this repo.
- To change parser behavior, edit `../bms-parser-cpp`, run `make clean && make test && make test_amalgamation` there, then copy `../bms-parser-cpp/build/bms_parser.hpp` and `../bms-parser-cpp/build/bms_parser.cpp` into `src/`.
- Commit and push the parser repo when parser behavior changes.

## Shader Compilation

- Shader sources live under `shader_src/` and are compiled by `shader_src/make.py`.
- The local bgfx shader compiler is:
  `bgfx/bgfx/.build/osx-arm64/bin/shadercRelease`
- To compile shaders from this machine, run:
  `cd shader_src && SHADERC=../bgfx/bgfx/.build/osx-arm64/bin/shadercRelease python3 make.py`

## Local Build Verification

- For desktop local compile checks, use the existing `cmake-build-debug` folder:
  `cmake --build cmake-build-debug --target main -j 6`
