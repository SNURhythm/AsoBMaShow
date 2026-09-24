# Build storage reduction

## Intent and scope

Reduce persistent desktop CMake and Android build storage while retaining fast
incremental builds, current optimization settings, full debug information, and
the existing test isolation. The user approved proceeding with the recommended
first phase: compatible test-object reuse and Android dependency consolidation.
This document is the architecture for that phase; implementation follows review.

Work stays in the current checkout and branch. No worktree, distribution upload,
parser behavior change, shader change, or iOS build-policy change is involved.
Reduced debug information, shared test DLLs, combined test executables, a global
compiler cache, and general-purpose automatic cleanup are outside this phase.

## Measured starting point

Read-only inspection on 2026-09-24 found:

| Area | Size or count |
| --- | --- |
| `cmake-build-debug` | 9.1 GiB |
| Its `CMakeFiles` directory | 6.0 GiB |
| Root-level test executables | 356 files, approximately 2.1 GiB |
| Application-source compilation entries | 3,752 entries for 451 unique sources |
| Repeated application objects with matching normalized commands | Approximately 1.8 GiB |
| Other desktop build directories | Approximately 2.4 GiB |
| `android/app/.cxx` | 7.1 GiB across four native configurations |
| `android/app/build` | 3.6 GiB |
| Android dependency installations | Four copies of 842–863 MiB each |

The command comparison ignored object/dependency output arguments but retained
compiler flags and include paths. It identifies candidates, not proof that every
candidate can be merged unchanged. Sizes are an observed baseline, not guaranteed
filesystem space recoverable: stale files and filesystem sharing affect totals.

The largest repeated sources include `bms_parser.cpp`, `ScoreProvenance.cpp`,
`ArchiveFile.cpp`, and `LuaSkinTableDecoder.cpp`. `records_test_support` already
demonstrates compile-once support, but many targets still list sources directly.

## Desktop: explicit reusable compilation targets

Add a focused CMake module for reusable test dependencies. Start with parser and
score/result primitives, then archive support and Lua decoding where the existing
compile settings match. Wire consumers explicitly in `CMakeLists.txt` and the
relevant `cmake/*Tests.cmake` files. Do not infer production build rules from a
previous `compile_commands.json` or automatically rewrite arbitrary targets.

Use small `OBJECT` targets for source groups whose consumers require every object.
Keep `STATIC` support targets where archive member selection is necessary for
independent fixtures or optional dependencies. Avoid one monolithic support
object library: injecting unused objects can introduce unresolved symbols or
collisions with fixture implementations.

Each migrated source has one owner per compatible compile variant. Remove it
from a consumer's direct source list when linking the replacement dependency;
ensure there is no second route to the same object through another support target.
An existing support archive may remain an archive rather than changing its
linking behavior solely to save its comparatively small archive copy.

Compatibility includes compiler/toolchain, platform, architecture, language
standard, include order and resolved headers, definitions, optimization, debug
information, assertions, runtime library, PIC, and sanitizer settings. Usage
requirements inherited from linked dependencies must be included in the audit.
Where settings differ, retain a separate explicit variant or leave that consumer
unchanged. Do not erase differences merely to increase sharing.

In particular:

- `main` receives `-O1` for Debug; most tests do not. Keep application objects
  separate in this phase unless their full settings already match.
- Test registration adds `SDL_MAIN_HANDLED` and undefines `NDEBUG`. Moving code
  into a library must preserve that source's prior assertion behavior in every
  configuration, including differences in existing support libraries.
- `ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS` requires an archive-test variant.
- Lua feature definitions and skin-resource test hooks remain distinct.
- Extracted/generated test fixtures and fixture-provided stubs remain local.
- Parser sources are only re-owned by CMake targets, never edited.

Preserve test names, commands, runtime dependency staging, working directories,
and process isolation. Keep platform conditions intact for Windows and mobile;
local macOS validation must not be represented as validation of those platforms.

## Android: one dependency installation per dependency identity

Keep AGP's native object directories separate by configuration. Move only the
vcpkg installation to a checkout-local directory beneath the already ignored
`.cache/android-vcpkg/<identity>/installed` and set `VCPKG_INSTALLED_DIR` before
the first CMake `project()` call. Debug and release can share this installation
because vcpkg installs the corresponding library configurations together.

Canonicalize the vcpkg and NDK paths in `android/app/build.gradle` before passing
them to CMake. Existing caches contain both `/ndk/` and `//ndk/` spellings.
Changing the path spelling must not create a different dependency identity.

Calculate a deterministic identity from the dependency inputs, including:

- Manifest and optional registry configuration contents and selected features.
- Target and host triplets, including local triplet file contents.
- Overlay port contents, using sorted relative paths and file contents.
- Canonical vcpkg location and revision/tool identity, including local changes to
  dependency-affecting scripts or triplets used by this configuration.
- Canonical NDK location and revision, Android API, ABI, STL, and chainload
  toolchain settings that affect dependency compilation.

Use a versioned identity schema so changes to the calculation cannot accidentally
reuse an installation with weaker compatibility checks. Do not key on app source
revision, version code, distribution flavor, or Debug versus RelWithDebInfo when
those do not change dependency compilation. Preserve vcpkg's own ABI checks and
binary-cache behavior; this directory scheme does not replace them.

Acquire a CMake process lock in the selected identity directory before the
top-level `project()` invokes the vcpkg toolchain; release it after initialization
finishes. Encapsulate selection/locking in an Android-only CMake module invoked
before `project()`. Configure failures release the process lock. Use a finite,
documented timeout and print the lock path on failure. Compiler try-compile
projects must not recursively acquire the top-level lock.

This covers configurations launched by the deploy wrapper, Gradle directly, and
Android Studio. Different dependency identities use separate installations;
same-identity installations serialize. Compilation resumes without holding the
install lock. Do not rely only on a lock in `android_firebase_deploy.sh`.

Missing or invalid dependency inputs fail configuration with a useful error;
do not silently fall back to a globally shared mutable dependency tree. Record
the identity inputs alongside the installation for diagnosis. The shared path
stays outside Gradle's temporary `build` directory and ordinary clean operations.

## Migration and storage accounting

Reconfiguration introduces new objects and may temporarily increase storage.
Record a baseline manifest of generated outputs before migration, and compare it
with the new build graph after verification. Report both the size of live outputs
and retained obsolete artifacts; a successful refactor alone does not reclaim
the old files.

After verified builds, obsolete test objects may be removed only when their exact
paths came from the old generated graph, are inside the selected build directory,
and are absent from the new graph. Never clean the entire active build tree as a
substitute for identifying retired outputs. Do not delete files while builds run.

Identify Android configuration directories using their metadata and references
from the current Gradle build, not their hash names or age alone. List superseded
configurations and old dependency installs with sizes. Broad retirement of old
Android/desktop configurations remains a separate explicit cleanup action;
this phase does not remove optional release or feature build directories.

The measured 1.8 GiB of desktop duplicate objects is an opportunity ceiling for
the matching-command group, not a promised first-patch saving. Consolidating all
four current Android installations represents approximately 2.5 GiB of duplicate
dependencies. Removing old Android configurations overlaps with that figure and
must not be counted a second time.

## Validation and acceptance

For desktop, capture compile commands and test inventory before changing the
graph. Build every affected test target, not only `main`; then run:

```sh
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Compare source ownership and effective compilation settings before and after.
Verify that migrated compatible groups compile once, hook variants remain
separate, and the CTest inventory is preserved. Run representative affected tests
under a release configuration to check assertion behavior. Verify a second build
does not recompile the shared sources unnecessarily. Use the existing build
directories and keep output from unrelated existing failures distinguishable.

For Android, use only the documented build-only entry points:

```sh
scripts/android_firebase_deploy.sh --build-only
scripts/android_firebase_deploy.sh --build-only --variant playDebug
```

Confirm both generated CMake caches use the same dependency identity when their
dependency inputs match, but separate native object directories. Confirm the
second configuration reuses the installation. Exercise identity stability under
equivalent path spellings and invalidation under manifest, overlay, and toolchain
changes using small fixtures. Exercise same-identity lock contention and failure
release without running competing installs against the live cache. Preserve NDK
28.2.13676358, signing rules, automatic version codes, and Firebase/Play behavior.

Completion requires passing relevant builds/tests, measured reduction in live
duplicate artifacts, an accounting of retained obsolete files, and committed and
pushed task-only changes. No distribution action is part of validation.

## References

- [CMake object and static libraries](https://cmake.org/cmake/help/latest/command/add_library.html)
- [vcpkg CMake integration and installation path](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration)
- [Android CMake staging directory](https://developer.android.com/reference/tools/gradle-api/8.10/com/android/build/api/dsl/Cmake)
