# Android Build Notes

This Android target is intentionally folder-permission based, not copy based.
Chart folders selected with `ACTION_OPEN_DOCUMENT_TREE` are stored as persisted
tree URIs and exposed to native code as synthetic paths under `@androidtree@`.
Native code must read those paths through the Android bridge instead of
`std::filesystem`.

Firebase builds may use a direct external-storage path only on Android 11+
after all-files access is actually granted. Android 9/10 and Play builds keep
the selected SAF tree grant; they do not request the Android 11-only settings
flow or assume legacy storage authorization.

Large files should not be copied out of the selected tree. Chart text, images,
and sounds are read through `ContentResolver` on demand. Video files are opened
through a read-only Android file descriptor and passed to FFmpeg as
`/proc/self/fd/<fd>`. Archive files inside SAF folders are not expanded during
library scans, because doing so would require copying or a streaming archive
backend.

Explicit archive imports copy into a private inbox before import. Each archive
copy is capped at 8 GiB and checks that each write leaves at least 256 MiB of
usable storage. These limits do not cap folder imports or the entire library.
Storage availability is not reserved against concurrent writers. Read, write,
close, cancellation and budget failures remove that attempt's partial file;
an oversized package must be handled manually. Find BMS expansion has separate
member/total limits documented in `docs/find-bms-archive-limits.md`.

Renderer order on Android is Vulkan first, then OpenGLES fallback. Package both
`shaders/spirv` and `shaders/essl` into the APK.

Build from the repository root with a configured Android SDK/NDK and `VCPKG_ROOT`:

```sh
export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
export ANDROID_SDK_ROOT=$ANDROID_HOME
export VCPKG_ROOT=/Users/xf/vcpkg
android/gradlew -p android assembleDebug
```

For the same build through the Firebase helper without uploading:

```sh
scripts/android_firebase_deploy.sh --build-only
```

To deploy to Firebase App Distribution, copy
`scripts/android_firebase_deploy.env.example` to a private env file or set the
same values in your shell, then run:

```sh
scripts/android_firebase_deploy.sh --env-file /path/to/private.env
```

Deployment requires `FIREBASE_ANDROID_APP_ID` plus Firebase CLI auth. Leave
`ANDROID_VERSION_CODE` empty for automatic versioning. Build-only and deploy
runs both use a compact UTC timestamp version code. `firebaseRelease` and
`playRelease` builds also require release signing env values:
`ANDROID_KEYSTORE_PATH`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, and
`ANDROID_KEY_PASSWORD`. The same signing config is used for Firebase and Google
Play release builds. In GitHub Actions, these values are supplied by secrets on
the Android beta deploy job. The script builds first, then uploads the APK with
`firebase appdistribution:distribute`.

Before building after shader changes, generate all shader profiles:

```sh
cd shader_src
SHADERC=../bgfx/bgfx/.build/osx-arm64/bin/shadercRelease python3 make.py
```

## Platform boundary smoke tests

Both flavors permit legacy public HTTP table/archive hosts, including
user-configured hosts that cannot be covered by a fixed domain allowlist.
HTTP content can be observed or modified in transit; prefer HTTPS sources.
The network-security configuration changes only cleartext admission, not
trusted certificate authorities or TLS verification. Application-level
HTTPS-origin downgrade rejection and authenticated IR's HTTPS/origin checks
remain in force. Android's default for apps targeting API 28+ is to deny
cleartext unless explicitly enabled; see the
[Android network-security documentation](https://developer.android.com/privacy-and-security/security-config#CleartextTrafficPermitted).

The framework-only `PlatformBoundaryInstrumentation` is packaged only in the
test APK. With the usual SDK, Java 17, vcpkg and release-signing environment,
build it using `android/gradlew -p android :app:assembleFirebaseReleaseAndroidTest
-PandroidBoundaryTestBuildType=release`. Use `assemblePlayReleaseAndroidTest`
for Play. Build the matching application with the deployment helper's
`--build-only` option; no upload is needed.

On an already booted, unlocked test emulator, run
`python3 tests/android_platform_boundary_smoke.py --adb /path/to/adb --apk
/path/to/app.apk --test-apk /path/to/test.apk --flavor firebase` (or `play`).
The test installs both APKs without clearing application data, exercises the
actual activity HTTP connector with local HTTP/TLS fixtures, and verifies the
assembled target policy and SAF-only direct-path rejection. Its temporary CA
is trusted only inside the instrumentation run; production TLS trust is not
modified. The runner checks HTTP table/archive reads, HTTP-to-HTTPS upgrades,
HTTPS-to-HTTP rejection, and that the rejected destination receives no request.
Host-JVM lifecycle and copy regressions run through the existing Python tests.

The optional instrumentation mode `seed-saf` creates a small chart in
`Download/AsoBMaShowTask5Saf`; use it only in an isolated test user. Launch the
test-only `SafGrantActivity`, select that folder, then run mode `saf` with
`deliverSelection=1`. It delivers the real grant through the production
activity result handler and verifies synthetic-path listing and descriptor
reads. Rerun mode `saf` without that flag after stopping the target process to
verify persisted access. This bridge test does not initialize the native player
profile or claim full SQLite library indexing.
