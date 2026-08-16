# Build, release, and verification

## Intent

Build scripts and CI produce reproducible platform artifacts while keeping
distribution credentials, signing values, and private environment files out of
the repository. Local verification should compile and exercise the intended
surface without accidentally uploading an artifact.

## Build map

- Root `CMakeLists.txt` configures the portable app, platform options, shader
  copy rules, and standalone test registration; `src/CMakeLists.txt` lists
  application sources.
- `scripts/macos_init.sh`, `scripts/ios_init.sh`, and platform deployment
  wrappers prepare native dependencies and build directories.
- `scripts/android_firebase_deploy.sh` and `scripts/ios_firebase_deploy.sh`
  are the only local Firebase App Distribution entry points.
- `scripts/ios_release_verify.sh`, artifact-audit scripts, and
  `.github/workflows/` express release verification and distribution gates.
- `shader_src/make.py` compiles source shaders with the local bgfx compiler.

## Operational boundaries

Real `.env` files and signing values are private and must remain untracked.
Firebase deploy commands upload builds; use their `--build-only` forms for a
local compile check. TestFlight uses a separate serialized lane. The Android
and iOS build scripts select the required toolchain/runtime configuration;
avoid bypassing them with direct distribution lanes.

## Verification

- Desktop: `cmake --build cmake-build-debug --target main -j 6`.
- iOS non-distribution release check: `scripts/ios_release_verify.sh`.
- iOS Firebase compile-only: `scripts/ios_firebase_deploy.sh --build-only`.
- Android Firebase compile-only: `scripts/android_firebase_deploy.sh --build-only`.
- Shader compilation: run `shader_src/make.py` with the configured local
  `shadercRelease` path described in `AGENTS.md`.

Run focused tests for the changed feature before a broader CTest/build check.
Artifact and release-script verifiers are retained because they execute real
scripts or inspect generated package fixtures; do not replace them with source
string checks.

## Related pages

- [Mobile and platform integration](mobile-and-platform-integration.md)
- [Gameplay skins](gameplay-skins.md)
- [Audio, video, and display](audio-video-and-display.md)

## Authoritative operational references

- [`AGENTS.md`](../../AGENTS.md) contains repository-specific deployment,
  parser, shader, and emulator instructions.
- [iOS first-release checklist](../ios-first-release-checklist.md) and
  [release-readiness audit](../release-readiness-audit-2026-08-01.md) retain
  release evidence and manual gates.
