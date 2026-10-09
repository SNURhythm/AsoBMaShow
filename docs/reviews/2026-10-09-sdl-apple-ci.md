# SDL fork Apple CI repair

The SDL fork's [failed run](https://github.com/SNURhythm/SDL/actions/runs/37884447562)
at `3d22d98cf` failed in the iOS and tvOS CMake builds. iOS compiled an
unguarded `UIWindow.windowScene` access with an iOS 11 deployment target.
tvOS compiled an orientation helper using APIs unavailable on that platform.
After isolating the helper, the tvOS 11 build also exposed the unguarded
`windowScene` access.

SDL commit `75f164d12819a329e8f107f34db0146ac645ddb9` fixes both paths:

- Guard the orientation helper's declaration and definition out of tvOS and
  visionOS, matching its existing callers.
- Check API availability before accessing `windowScene`, retaining the legacy
  external-screen fallback for tvOS 11/12.
- Set the generated iOS CI job and all four iOS release-package checks to iOS
  15, matching the framework's existing Xcode settings and the approved minimum.
  Update the fork's iOS documentation accordingly. tvOS remains at 11.

## Verification

- Reproduced the affected UIKit object compilation failures before the fixes.
- Full arm64 CMake builds passed for iOS 15 and tvOS 11 with shared/static SDL,
  tests and examples enabled, and `SDL_WERROR=ON`.
- Xcode Release framework builds passed for iOS 15 and tvOS 15 with signing
  disabled. Local Xcode 27.1 rejects a tvOS 11 deployment target before source
  compilation; only this local Xcode invocation used the tvOS 15 override.
  The CMake tvOS build retained 11, as does the committed CI configuration.
- CI test-plan generation and whitespace checks passed. Independent review
  found no blocking issues in the seven-file SDL change.
- The application's existing macOS `main` target rebuilt successfully against
  the updated submodule.
- Both **iOS (CMake & xcode)** and **tvOS (CMake & xcode)** jobs passed in
  [GitHub Actions run 37892159447](https://github.com/SNURhythm/SDL/actions/runs/37892159447)
  at the fixed SDL commit. This confirms the committed iOS 15 and tvOS 11
  configurations on the CI runners, including packaging and consumer checks.

These are compile checks, not iOS/tvOS runtime tests. Local builds used iOS
SDK 27.1 and tvOS SDK 27.0. This repair does not establish full visionOS support.
