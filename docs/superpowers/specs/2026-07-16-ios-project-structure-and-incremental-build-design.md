# iOS Project Structure and Incremental Build Design

## Goal

Make the iOS app target discover new source files automatically and restore
incremental local and Firebase PR builds without changing the intentionally
clean TestFlight archive path.

## Current Problems and Evidence

The Xcode project already represents `src/` as a
`PBXFileSystemSynchronizedRootGroup`, but the group is not registered in the
app target's `fileSystemSynchronizedGroups`. Instead, 188 paths are stored in a
`membershipExceptions` array as an effective source allow-list. Adding a new
source file therefore requires a manual project-file edit even though the
folder is synchronized.

The CocoaPods setup also contains two machine-specific structures:

- `ios/Xcode/AsoBMaShow/Pods` is a tracked absolute symlink into
  `/Users/xf/Library/Caches`.
- `AsoBMaShow.xcworkspace` references the Pods project both through
  `group:Pods/Pods.xcodeproj` and through an absolute cache-relative path.

Two consecutive non-signing `xcodebuild` builds using the same DerivedData
directory still scheduled 103 and 124 `CompileC` actions. The generated build
manifests alternated between two identities for the same Pods targets, which
also alternated their VFS header-map paths. Those changing paths invalidate
application compilation even when no source changed. The build database also
contains app compile tasks from several Git worktrees sharing the one global
`AsoBMaShow-FirebaseCI` DerivedData directory, so switching worktrees replaces
task signatures and prevents reliable reuse.

This reproduces through plain `xcodebuild`; Fastlane is not the limiting
component. Fastlane's `build_app` already exposes the `clean` and
`derived_data_path` controls needed by this design.

## Constraints

- New supported source files under `src/` must join the iOS app target without
  editing `project.pbxproj`.
- `AndroidNatives.cpp` must remain outside the iOS target.
- `audio/AudioWrapper.cpp` must retain its explicit Objective-C++ file type.
- Existing frameworks, libraries, resources, build settings, signing, and
  export behavior must remain unchanged.
- TestFlight builds must continue to use `clean: true`.
- Firebase PR archives and `scripts/ios_firebase_deploy.sh --build-only` must
  retain a stable DerivedData directory and reuse `ArchiveIntermediates` or
  normal build intermediates within the same checkout.
- Separate checkouts and Git worktrees must not share DerivedData by default.
- `IOS_DERIVED_DATA_PATH` must remain an explicit override.
- CocoaPods installation must remain lockfile-controlled and must not place a
  tracked machine-specific path in the repository.
- Verification must not upload a build.

## Chosen Approach

Use Xcode's native buildable synchronized-folder model, a portable local Pods
directory backed by the existing external cache, and a checkout-scoped
DerivedData path.

Generating the whole app project with CMake was rejected because it would also
require migrating resources, framework references, CocoaPods integration,
signing, schemes, and archive behavior. Generating the existing 188-entry
allow-list was rejected because it would preserve project-file churn and a
second source-of-truth mechanism.

## Xcode Source Membership

Register the existing `PBXFileSystemSynchronizedRootGroup` for `src/` in the
`AsoBMaShow` native target's `fileSystemSynchronizedGroups`. Once the target
owns the folder, Xcode automatically classifies and builds supported files
that appear below it.

Replace the current allow-list-shaped exception set with the actual target
exception: `AndroidNatives.cpp`. Keep the existing explicit file-type entry
that compiles `audio/AudioWrapper.cpp` as Objective-C++.

Headers do not need build-phase entries. Platform implementations that are
already compiled on iOS behind preprocessor guards retain their current
behavior. This change affects only how Xcode discovers files; it does not
replace the explicit CMake source lists used by desktop and test builds.

Existing architecture checks that look for individual filenames in
`membershipExceptions` will instead verify that the `src/` synchronized group
belongs to the app target and that the Android native source is excluded.

## CocoaPods Layout and Cache

Delete the tracked `Pods` symlink and leave `Pods/` ignored. Keep exactly one
workspace reference: `group:Pods/Pods.xcodeproj`. No absolute user, cache, or
worktree path may be written to the workspace.

The external lock-hashed cache remains an optimization, but it is no longer
mounted into the project as a symlink. `ios_init.sh` follows this flow:

1. Hash `Podfile.lock` and `Gemfile.lock` to select the existing cache key.
2. If the cache contains a matching `Manifest.lock` and a complete
   `Pods.xcodeproj`, restore it into a normal local `Pods/` directory while
   preserving timestamps and skip `pod install`.
3. If the cache is absent or invalid, remove any partial local Pods directory,
   run `bundle exec pod install --deployment` locally, validate the generated
   manifest and project, and refresh the external cache.
4. Fail the initialization if installation or cache refresh is incomplete;
   never leave a successful-looking partial Pods directory.

This retains fast setup while giving Xcode one stable, checkout-relative
project identity. It also prevents `pod install` from rewriting the app
workspace during every unchanged build.

## DerivedData Isolation

Add one repository helper that resolves the DerivedData directory. Resolution
uses this precedence:

1. A non-empty `IOS_DERIVED_DATA_PATH` value exactly as supplied by the user or
   runner.
2. `${HOME}/Library/Developer/Xcode/DerivedData/AsoBMaShow-FirebaseCI-<id>`,
   where `<id>` is a short deterministic hash of the canonical repository root.

The same checkout therefore reuses the same build database across runs, while
different Git worktrees receive different directories. The helper is shared
by the build-only shell path and Fastlane so they cannot silently diverge.

Only Firebase builds receive this explicit DerivedData path in Fastlane.
TestFlight retains its current clean behavior and default archive handling.

## Failure Handling

- Missing or invalid Pods cache entries fall back to a local deployment-mode
  CocoaPods install.
- A failed CocoaPods install or cache refresh exits `ios_init.sh` nonzero.
- An unavailable canonical root or hash command exits the DerivedData helper
  nonzero rather than falling back to the unsafe global directory.
- An explicit `IOS_DERIVED_DATA_PATH` bypasses hashing and remains suitable for
  runner-controlled cache placement.
- No automatic cleanup deletes existing DerivedData. Old global cache content
  becomes unused and can be removed manually after the migration is proven.

## Repository Guards

Add a focused, fast repository check for the iOS build structure. It verifies:

- the app target owns the synchronized `src/` group;
- `AndroidNatives.cpp` is the only source-membership exclusion;
- the Objective-C++ override remains present;
- the workspace has exactly one relative Pods project reference;
- no tracked or project/workspace path contains the machine-specific Pods
  cache location;
- the build-only and Fastlane paths use the shared DerivedData resolver; and
- TestFlight's clean condition is unchanged.

Update the existing application-startup and result-persistence checks so they
depend on synchronized target membership instead of requiring individual
source names in the project file. Update `AGENTS.md` to remove the manual
source-list instruction and document checkout-scoped incremental build state.

## Verification

Implementation follows a configuration-focused red/green cycle:

1. Add the repository guard and confirm it fails against the current manual
   membership, duplicate workspace reference, tracked symlink, and global
   DerivedData default.
2. Apply the project, Pods, and DerivedData changes until the guard passes.
3. Run the existing startup and result-persistence audit scripts.
4. Run `ios_init.sh` twice and confirm the tracked Xcode project and workspace
   checksums do not change on the second run.
5. Use a fresh checkout-scoped DerivedData directory for one non-signing
   `scripts/ios_firebase_deploy.sh --build-only` build.
6. Repeat the same build without source changes and confirm the app target has
   zero `CompileC` actions. Always-out-of-date third-party or packaging phases
   may still run and the final app may relink; those are separate from source
   recompilation.
7. Run the repository's desktop compile check to ensure project-structure
   changes did not disturb the CMake build.

No Fastlane deployment lane is run during verification, so this work cannot
upload to Firebase or TestFlight.

## Out of Scope

- Removing the intentional TestFlight clean build.
- Replacing CocoaPods or Fastlane.
- Converting the entire app project to CMake generation.
- Refactoring SDL, SDL_ttf, bgfx, or their always-run build phases.
- Automatically deleting historical global DerivedData or CocoaPods caches.
