# Android Build Notes

This Android target is intentionally folder-permission based, not copy based.
Chart folders selected with `ACTION_OPEN_DOCUMENT_TREE` are stored as persisted
tree URIs and exposed to native code as synthetic paths under `@androidtree@`.
Native code must read those paths through the Android bridge instead of
`std::filesystem`.

Large files should not be copied out of the selected tree. Chart text, images,
and sounds are read through `ContentResolver` on demand. Video files are opened
through a read-only Android file descriptor and passed to FFmpeg as
`/proc/self/fd/<fd>`. Archive files inside SAF folders are not expanded during
library scans, because doing so would require copying or a streaming archive
backend.

Renderer order on Android is Vulkan first, then OpenGLES fallback. Package both
`shaders/spirv` and `shaders/essl` into the APK.

Build from the repository root with a configured Android SDK/NDK and `VCPKG_ROOT`:

```sh
export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
export ANDROID_SDK_ROOT=$ANDROID_HOME
export VCPKG_ROOT=/Users/xf/vcpkg
SDL/android-project/gradlew -p android assembleDebug
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
