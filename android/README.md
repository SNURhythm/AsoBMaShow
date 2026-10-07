# Android Build Notes

The default `restricted_file_access` build copies imported folders into the app's
Documents/BMS directory. Chart loading continues to use native filesystem paths,
so loading thousands of WAV files does not require individual SAF calls. Import
Archive uses the same managed library. Both flavors use the same Documents
subtree and application ID.

Android's **Manage files** button offers folder import, archive import and
**Open in Files**, with a short explanation of each. Folder import offers Copy
(keep the originals) or Move. Move frees space after each subfolder is fully
copied and synced, then removes that source subfolder before continuing. It needs
space for the current subfolder, rather than another copy of the entire tree.
Tasks shows discovery, copied files/bytes, source removal and library indexing.
If a move is interrupted or source deletion fails, completed destination copies
remain in BMS and remaining source content is kept; the task reports the failure.

**Open in Files** always uses the system Files browser, opening Documents/BMS.
Navigate up to reach the Documents root. Documents/Skins is created when missing.
The AsoBMaShow root exposes the entire Documents directory, including
BMS, databases, Skins and other user files. Files-aware apps can browse, copy and
export these files through Android's permission grants. The db and profiles
subtrees are read-only through the provider; other files can also be edited,
renamed and deleted. The provider uses `getExternalFilesDir(null)/Documents` with `getFilesDir()/Documents`
as fallback, matching the native Documents path. Rendered music and archive
indexes use Android's private cache. Import staging, credentials, private skin
state and shared preferences stay outside the exposed Documents subtree.
This testing layout starts fresh; no migration from the old flat layout is run.

Put charts under BMS. Import Folder rejects the app's own Documents tree to avoid
recursively copying it into itself. Provider changes in BMS are coalesced until writable handles
close and changes settle; the next foreground session queues a library refresh.
Pending changes survive an app restart and clear after the refresh succeeds.
The db and profiles subtrees remain read-only even when the game is closed;
Files supports exporting them, not replacing databases. Other files remain
directly editable, and skin changes may require reopening the app.

`all_file_access` retains Add Folder for users who want direct external folders.
On Android 11+ it requests all-files access before opening the folder picker.
Legacy persisted `@androidtree@` folders remain readable through the SAF bridge;
they are not the default import workflow because each resource read uses SAF.
This flavor's permission policy is independent of Firebase distribution.

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
android/gradlew -p android assembleRestricted_file_accessDebug
```

The Firebase helper defaults to `restricted_file_accessRelease`. Build without uploading:

```sh
scripts/android_firebase_deploy.sh --build-only
```

To explicitly build with Android all-files access, use
`scripts/android_firebase_deploy.sh --build-only --variant all_file_accessRelease`.
The restricted flavor omits `MANAGE_EXTERNAL_STORAGE` regardless of where the
APK is distributed.

To deploy to Firebase App Distribution, copy
`scripts/android_firebase_deploy.env.example` to a private env file or set the
same values in your shell, then run:

```sh
scripts/android_firebase_deploy.sh --env-file /path/to/private.env
```

Deployment requires `FIREBASE_ANDROID_APP_ID` plus Firebase CLI auth. Leave
`ANDROID_VERSION_CODE` empty for automatic versioning. Build-only and deploy
runs both use a compact UTC timestamp version code. `restricted_file_accessRelease` and
`all_file_accessRelease` builds also require release signing env values:
`ANDROID_KEYSTORE_PATH`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, and
`ANDROID_KEY_PASSWORD`. The same signing config is used for Firebase and Google
Play release builds. In GitHub Actions, these values are supplied by secrets on
the manual **Build & Deploy Android Beta (Manual)** workflow in
`.github/workflows/android-beta-deploy.yml`. Start it from GitHub Actions using
**Run workflow** and select the branch to build; pushes do not deploy Android.
The script builds first, then uploads the APK with
`firebase appdistribution:distribute`.

Before building after shader changes, generate all shader profiles:

```sh
cd shader_src
SHADERC=../bgfx/bgfx/.build/osx-arm64/bin/shadercRelease python3 make.py
```

## Platform boundary smoke tests

The Android application configures `TMPDIR` and `SQLITE_TMPDIR` to its private
cache before the SDL activity starts. Native downloads and archive processing
use C++ temporary storage, and bundled SQLite needs temporary files during
schema migration. libc++'s default `/data/local/tmp` and Android's working
directory are not writable app storage. The override keeps temporary data
private without moving profile databases or forcing large operations into RAM.
The platform smoke runner verifies both environment variables and a bounded
owned cache-file probe before its network checks.

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
build it using `android/gradlew -p android :app:assembleRestricted_file_accessReleaseAndroidTest
-PandroidBoundaryTestBuildType=release`. Use `assembleAll_file_accessReleaseAndroidTest`
for all-files access. Build the matching application with the deployment helper's
`--build-only` option; no upload is needed.

On an already booted, unlocked test emulator, run
`python3 tests/android_platform_boundary_smoke.py --adb /path/to/adb --apk
/path/to/app.apk --test-apk /path/to/test.apk --flavor restricted_file_access` (or `all_file_access`).
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

## Documents provider checks

Run `python3 tests/android_documents_provider_tests.py` for path containment and
copy-batch coalescing. After installing the matching app and instrumentation APK,
run:

```sh
adb shell am instrument -w -e mode documents-provider \
  com.snurhythm.asobmashow.test/com.snurhythm.asobmashow.PlatformBoundaryInstrumentation
```

This checks full-root browsing, CRUD, truncation, native-path equivalence and
root/path protection. It removes only its uniquely named fixture folder and
cache sentinel. Mode `documents-refresh` runs those checks, launches the app and
also waits for the native library refresh to complete and clear the persisted
pending-change flag. These checks also cover SAF folder Copy/Move and the system
Files launch intent. Mode `documents-open-files` opens the production BMS link
in system Files for manual navigation checks. Run
`python3 tests/android_folder_import_tests.py` for incremental move, interruption,
source-change, deletion-failure and progress cases without a device.
