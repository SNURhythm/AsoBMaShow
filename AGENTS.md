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
- Firebase PR builds should keep checkout-specific DerivedData and the stable Firebase archive object root beneath it. Xcode does not preserve its default archive object location across separate `archive` invocations. `IOS_DERIVED_DATA_PATH` remains available as an explicit override.
- The iOS app target uses an Xcode file-system-synchronized group and automatically discovers supported files under `src`. Keep platform-only and build-metadata exclusions in the synchronized group's small `membershipExceptions` list; do not add normal new source files there.

## Android Firebase Deploy

- To deploy an Android build to Firebase App Distribution from this machine, use:
  `scripts/android_firebase_deploy.sh`
- For a fast local release compile check without upload, run with Android signing env configured:
  `scripts/android_firebase_deploy.sh --build-only`
- The Android deploy script loads `.env`, `.env.local`, `android/.env`, and `android/.env.local`.
- The script defaults to the `firebaseRelease` flavor and will use `/usr/libexec/java_home -v 17` on this machine if the active shell Java is too new for Gradle.
- Use `scripts/android_firebase_deploy.env.example` as the private env template. Real env files must stay out of git.
- The script can infer `FIREBASE_ANDROID_APP_ID` and `FIREBASE_PROJECT` from `android/app/google-services.json`.
- Leave `ANDROID_VERSION_CODE` empty unless the user explicitly wants an override. Build-only and deploy runs both use an automatic compact UTC timestamp version code; the script does not query Firebase releases for versioning.
- Android release builds require `ANDROID_KEYSTORE_PATH`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, and `ANDROID_KEY_PASSWORD`. The same release signing config is used for Firebase App Distribution and Google Play builds. Keep real values in private env files, runner environment, or GitHub Actions secrets only.
- Firebase Android builds request `MANAGE_EXTERNAL_STORAGE`; Play builds opt out via the `play` flavor. For a local Play compile check, run:
  `scripts/android_firebase_deploy.sh --build-only --variant playDebug`
- Debug variants remain debug-signed. Do not use debug signing for Firebase or Play release builds.
- Running the deploy script uploads a build. Only run it without `--build-only` when the user explicitly asks for deployment.
- GitHub Actions deploys Android from `.github/workflows/mobile-beta-deploy.yml` only for commits pushed to `develop`. The Android job has no dependency on the iOS/TestFlight job, so they can run in parallel when matching self-hosted runners are available. The job reads Android signing values from GitHub Actions secrets, and the self-hosted runner is expected to have an authenticated Firebase CLI session; do not add Android Firebase auth secrets unless the user asks.

## Android Emulator Testing

- The Homebrew Android SDK root on this machine is:
  `/opt/homebrew/share/android-commandlinetools`
- For headless smoke tests, prefer the `android10` AVD. `Pixel_7a_API_33_GooglePlay` has been observed to boot as `RUNNING_LOCKED` in headless mode and can reject app launches with a misleading `Activity class ... does not exist` error.
- Build first with Android signing env configured:
  `scripts/android_firebase_deploy.sh --build-only`
- Boot the emulator:
  `/opt/homebrew/share/android-commandlinetools/emulator/emulator -avd android10 -no-window -gpu swiftshader_indirect -no-snapshot -no-audio -no-boot-anim`
- When the user wants to see the emulator window, use the Android 13 AVD without `-no-window`:
  `/opt/homebrew/share/android-commandlinetools/emulator/emulator -avd Pixel_7a_API_33_GooglePlay -gpu host -feature -Vulkan -no-snapshot -no-audio -no-boot-anim`
- Wait for boot and confirm the user is unlocked:
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb wait-for-device`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell getprop sys.boot_completed`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell dumpsys user | sed -n '1,35p'`
- Install and launch with the explicit component. `monkey -p` may fail to resolve the launcher in headless emulator tests:
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb install -r android/app/build/outputs/apk/firebase/release/app-firebase-release.apk`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb logcat -c`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell am start --user 0 -n com.snurhythm.asobmashow/.AsoBMaShowActivity`
- Useful runtime checks:
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell pidof com.snurhythm.asobmashow`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell dumpsys activity activities | rg -n "ResumedActivity|com.snurhythm|documentsui|AsoBMaShow"`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell logcat -d -v time | rg -i "AsoBMaShow|SDL|bgfx|AndroidRuntime|FATAL EXCEPTION|Fatal signal|tombstone|ANR|renderer|Vulkan|OpenGLES"`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb exec-out screencap -p > /tmp/asobmashow-android.png`
- In Firebase builds, Add Folder requests Android all-files access first, then opens `ACTION_OPEN_DOCUMENT_TREE`; if access is granted and the picker returns primary external storage, the app stores a direct `/storage/emulated/0/...` path instead of an `@androidtree@` SAF path. In logs, a good real-device graphics startup includes `bgfx renderer: Vulkan`; the Android emulator may intentionally use OpenGL ES.
- Archive import can be tested without tapping UI by pushing a zip to Downloads, resolving its MediaStore `content://media/external/file/<id>` URI, then launching either:
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell am start --user 0 --grant-read-uri-permission -a android.intent.action.VIEW -d content://media/external/file/<id> -t application/zip -n com.snurhythm.asobmashow/.AsoBMaShowActivity`
  `/opt/homebrew/share/android-commandlinetools/platform-tools/adb shell am start --user 0 --grant-read-uri-permission -a android.intent.action.SEND -t application/zip --eu android.intent.extra.STREAM content://media/external/file/<id> -n com.snurhythm.asobmashow/.AsoBMaShowActivity`
  A successful import logs `Android archive import result: Imported archive. Library refreshed.` and extracts under the app's internal `files/ImportedCharts`.
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
