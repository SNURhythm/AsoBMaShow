# Choosing focused tests

Start with the behavior and subsystem you changed. Build and run its group;
add adjacent groups when a change crosses their boundary. A focused passing run
is normally enough for a localized change. The full suite is useful for broad
shared-header, dependency, compiler/linker, threading or persistence changes,
unclear impact, and release verification. It is not required after every edit
or before every commit. Documentation-only edits usually need no tests.

## Commands

Configure an existing build once after pulling changes to test registration:

```sh
cmake -S . -B cmake-build-debug
```

For a fresh checkout, use the repository's normal `cmake --preset debug` setup.
List available groups or preview a selection without running it:

```sh
ctest --test-dir cmake-build-debug --print-labels
ctest --test-dir cmake-build-debug -N -L '^area_audio$'
```

Build only the native targets needed by a group, then run its tests:

```sh
cmake --build cmake-build-debug --target tests_audio -j 6
ctest --test-dir cmake-build-debug -L '^area_audio$' --output-on-failure --no-tests=error -j 6
```

For multiple groups, combine build targets in **one** build invocation and use
a label union. Repeated `-L` flags mean intersection, not union.

```sh
cmake --build cmake-build-debug --target tests_audio tests_video -j 6
ctest --test-dir cmake-build-debug -L '^area_(audio|video)$' --output-on-failure --no-tests=error -j 6
```

For a small change with a known test, an exact `-R '^test_name$'` selection is
also appropriate. Build its native target first; aliases and shards may have
different target names, so use the group build when unsure. CTest automatically
includes required setup/cleanup fixtures. The group build includes their native
dependencies too. Python fixtures may compile additional temporary code when
they run. Labels select tests; they do not change timeouts or assertions.

For a multi-config generator, add `--config Debug` to the build command and
`-C Debug` to CTest, using that configuration's build directory.

## Subsystem map

Each row gives a group suffix: `audio` means build target `tests_audio` and
CTest label `area_audio`. Membership is explicit in
[`cmake/TestGroupCatalog.cmake`](../../cmake/TestGroupCatalog.cmake); some
boundary tests have more than one area label. Use `ctest -N -L` for the current
platform's exact membership, including required fixtures.

| Group | Source area / behavior | Often related |
| --- | --- | --- |
| `core` | `src/utils`, thread helpers, hashing, checksums, URL policy, math | Consumers of the changed helper |
| `app` | `Application*`, startup, event dispatch, lifecycle, scene navigation | `input`, `profiles`, `ui` |
| `audio` | `src/audio`, Jukebox, preview streaming, sound selection and handoff | `music_select`, `gameplay`, `video` |
| `video` | `src/video`, movie clocks, video playback lifecycle | `audio`, `rendering`, `replay` |
| `input` | `src/input`, touch, MIDI, gyroscope, timestamp ingress, capture | `gameplay`, `settings`, platform label |
| `gameplay` | `src/scene/play` rules, judge/gauge, simulation, startup, chart viewer | `input`, `practice`, `replay`, `skin_runtime` |
| `practice` | `src/practice`, ranges, presets, analytics, practice boundaries | `gameplay`, `results` |
| `results` | Result models, persistence/recovery, record lists/actions, score queries | `replay`, `ir`, `profiles` |
| `replay` | `src/replay`, replay repositories, codecs, storage, playback, export | `gameplay`, `results`, `video` |
| `ir` | `src/ir`, HTTP, Tachi, rankings, submissions, credentials | `results`, `profiles`, `music_select` |
| `library` | `src/library`, chart repositories/scanning, folder/document import, difficulty tables | `archives`, `downloads`, `music_select` |
| `archives` | `src/archive`, `ArchiveFile`, extraction, indexes, temporary archive cache | `library`, `downloads`, `skin_assets` |
| `downloads` | `src/bms_search`, Find BMS flow, metadata transport, verification | `archives`, `library`, platform label |
| `profiles` | `PlayerProfile*`, profile settings/archive/database ownership, provenance | `settings`, `results`, `ir` |
| `settings` | `src/settings`, settings scene models/controls and initialization | The subsystem whose setting changes |
| `music_select` | `src/music_select`, MainMenu library browsing, preview, launch, skin bridge | `library`, `audio`, `skin_runtime` |
| `skin_assets` | Skin packages/paths/import, resources, caches, lifecycle, settings, acceptance | `skin_runtime`, `rendering`, `archives` |
| `skin_runtime` | Lua hosts/bindings, JSON/LR2 decoders, play/result skin sessions and bridges | `skin_assets`, `rendering`, `music_select` |
| `rendering` | `src/rendering`, bgfx, shaders, images, skin normalization/drawing, display timing | `skin_runtime`, `ui`, `video` |
| `ui` | `src/view` controls/layout, scrolling, text entry, localization, navigation UI | The scene using the changed widget |
| `build` | CMake/test tooling, fixture compiler, build identity, shader workflow | A representative affected group |

Examples: a Jukebox-only fix starts with `audio`; a change to the preview/menu
handoff uses `audio` + `music_select`; replay persistence changes use `replay` +
`results`; shared skin parsing/rendering changes use `skin_runtime` +
`skin_assets` + `rendering`. A broadly used base view or database helper may
justify the full suite. These are starting points, not a dependency analyzer.

## Platform and release checks

`platform_ios`, `platform_android`, and `platform_windows` are additional
labels for host-side platform contracts/bridges. Build them with
`tests_platform_ios`, `tests_platform_android`, or `tests_platform_windows`:

```sh
cmake --build cmake-build-debug --target tests_platform_android -j 6
ctest --test-dir cmake-build-debug -L '^platform_android$' --output-on-failure --no-tests=error -j 6
```

These labels do not compile a mobile app or replace device verification.
Platform packaging/workflow tests outside CTest remain directly runnable:

- iOS: `python3 tests/ios_build_setup_tests.py`,
  `python3 tests/ios_release_workflow_tests.py`,
  `python3 tests/ios_artifact_audit_tests.py`.
- Android: `python3 tests/android_release_workflow_tests.py`,
  `python3 tests/android_play_workflow_tests.py`.
- macOS artifact packaging: `python3 tests/macos_artifact_audit_tests.py`.

For release-critical native tests plus an unsigned iOS build, use
`scripts/ios_release_verify.sh`. Follow `AGENTS.md` for platform compile checks
and emulator/device work. None of the group commands deploys anything.

When a full desktop run is warranted:

```sh
cmake --build cmake-build-debug -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

## Maintaining the groups

Add a new test to an appropriate `area_*` block in `TestGroupCatalog.cmake`.
CMake reports unclassified tests during configuration. Inactive platform tests
in the catalog are ignored. Preserve old test names and keep group membership
about behavior, not whether a test is native, Python, fast, or slow.

`asobmashow_register_test` records the native target even for aliases/shards.
A directly registered test named after its target is discovered automatically.
For a Python wrapper, a differently named native command, or a runtime helper
library, declare `asobmashow_test_build_targets(test_name target...)` in the
catalog. Required CTest fixtures are included transitively in group builds.
Tests in new CMake subdirectories need their own classification/finalization;
the current catalog covers tests registered in the repository root directory.
