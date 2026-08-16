# Mobile and platform integration

## Intent and user flow

The Android and iOS applications host the portable C++ runtime while supplying
platform permissions, storage/document access, lifecycle callbacks, media,
input, and package-specific release behavior. Desktop builds retain equivalent
portable behavior without taking a mobile dependency.

## Code map

- `android/app/src/main/java/com/snurhythm/asobmashow/` contains the Activity,
  document handoff, service, and Android-specific integration.
- `ios/Xcode/AsoBMaShow/` contains the Xcode target, native bridges, Fastlane,
  CocoaPods, and Apple resource packaging.
- `src/AndroidNatives.*`, `src/iOSNatives.*`, `src/platform/`, and platform
  branches in input/IR/media domains expose narrow portable interfaces.
- Document-handoff request/restore/rollback helpers coordinate imports and
  exports without making Android or iOS APIs leak into scenes.

## Boundaries and invariants

Platform code converts OS callbacks, URIs, and permissions into explicit
portable operations. Persisted URI access needs the minimum required grant;
export/restore attempts keep their write lease until rollback is complete.
Mobile storage paths remain private unless a deliberate user-selected location
is represented through a platform document contract. iOS source membership is
filesystem-synchronized; follow the repository instructions instead of adding
ordinary sources manually to Xcode project metadata.

## Verification

Use Android Java unit tests, `platform_document_handoff_tests`, Android/iOS
release workflow tests, and the platform build-only commands in the
[build guide](build-release-and-verification.md). For emulator testing and
distribution precautions, follow `AGENTS.md`.

## Related pages

- [Profiles and data transfer](profiles-and-data-transfer.md)
- [Input and controllers](input-and-controllers.md)
- [Build, release, and verification](build-release-and-verification.md)
