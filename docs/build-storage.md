# Build storage

Desktop tests share selected production objects through
`cmake/SharedTestDependencies.cmake`. The source and consumer lists are explicit;
`SharedTestSources.cmake` shares a source only between executables with matching
compile settings and link dependency requirements. Tests keep their own fixtures,
entry points, processes, and link dependencies. Full debug information remains
enabled. PIC/PIE, precompiled-header, exported, and target-sensitive configurations
retain their original compilations.

Add new candidates to the explicit source and consumer lists after comparing
their generated compile commands. Sources that are absent from a consumer are
not injected. Differing definitions, language settings, include paths, and
dependency requirements produce separate objects. Call the helper only after
all compile and link settings are finalized.

Android Gradle builds install vcpkg dependencies beneath
`.cache/android-vcpkg/<identity>/installed`, outside the per-configuration `.cxx`
directories. Compatible Debug and RelWithDebInfo builds reuse the installation;
their native object directories remain separate. The identity covers manifest,
overlay, triplet, vcpkg, NDK, and relevant configuration inputs. Each identity's
`inputs.txt` records the inputs and hashes for diagnosis.

Installation is serialized by CMake before `project()` and unlocked immediately
after initialization, including when builds originate in Android Studio. A
configure failure releases the process lock. The default lock wait is 600 seconds;
`ASOBMASHOW_DEPENDENCY_LOCK_TIMEOUT` is a CMake cache setting. A timeout reports
the lock path. The existing deploy script's `--build-only` mode remains the local
validation entry point and does not upload anything.

Changing the build graph does not delete old objects, `.cxx` configurations, or
old dependency identities. Measure retained artifacts separately from the live
build graph. Retire an old configuration only after identifying its consumers;
do not delete an installation referenced by an active build or clean the entire
desktop tree just to remove replaced objects. A new dependency identity also
invalidates CMake package-discovery paths referring to the previous installation.

The focused build-configuration tests run as part of CTest, or independently:

```sh
python3 tests/shared_test_sources_tests.py -v
python3 tests/android_dependency_cache_tests.py -v
```

## Validation and measured reduction (2026-09-24)

The desktop Debug compile database decreased from 4,277 to 3,794 entries
(483 fewer compilations). All 127 shared compilation commands matched their
original commands apart from output paths; all 443 application compilation
commands were unchanged. The 610 retired objects occupied 1,374.6 MiB, and their
127 replacements occupy 288.4 MiB: a net reduction of 1,086.2 MiB (1.06 GiB).
Only those retired outputs were removed. A subsequent build compiled no objects.

Firebase release and Play Debug build-only checks both passed and selected the
same 862.7 MiB Android dependency installation. Repeating Play Debug passed
without native compilation. Initial validation retained older Android configurations
and their four local dependency installations. The follow-up cleanup below
reclaimed those artifacts after the user requested it.

The full desktop build and two representative Release tests passed. Both new
build-configuration tests passed. Two full CTest runs at `-j 6` passed 403 of 404
cases; `chart_audio_renderer_tests` exceeded its existing 30-second timeout in
both parallel runs, passed in isolation in 12.94 seconds, and passed three more consecutive
isolated runs in 12.92–13.09 seconds. Its large audio mixing workload remains sensitive to concurrent load; no test timeout or fixture
was changed for this storage work.

## Follow-up artifact cleanup (2026-09-24)

Removed the four superseded native configurations (`Debug/35133n20` and
`RelWithDebInfo/{27exq3c4,2l6u21r3,4k51t406}`), their corresponding native outputs,
and old Firebase Debug / Play Release packaging artifacts. No Android build was
running. Current Firebase Release and Play Debug APKs, native build trees, and
shared dependencies were retained; their CMake caches and Ninja graphs did not
reference the removed directories.

| Measured area | Before cleanup | After cleanup |
| --- | ---: | ---: |
| Desktop Debug | 8.092 GiB | 8.092 GiB |
| Android `.cxx` | 9.870 GiB | 2.758 GiB |
| Android app `build` | 5.649 GiB | 2.903 GiB |
| Shared Android dependencies | 0.843 GiB | 0.843 GiB |
| Total | 24.454 GiB | 14.595 GiB |

Allocated artifact storage decreased by 9.858 GiB. Filesystem available space
increased from 9.863 to 19.687 GiB during cleanup. The final measured total is
approximately 5.2 GiB below the rounded 19.8 GiB initial baseline. The removed
variants will regenerate their packaging artifacts if built again, while reusing
the compatible native configurations and shared dependency installation.

## Additional desktop object sharing (2026-09-24)

Extended the explicit list by 124 C/C++ production sources while preserving the
helper and consumer list. Both local macOS Debug and Release graphs decreased
from 3,794 to 3,258 compilation entries. Each of the 741 retired compilations has
exactly one command-identical replacement on its original executable's Ninja
link edge, supplied by 205 additional shared objects. All 443 main compilation
commands and all 404 CTest registrations remain unchanged. Objective-C++ and
build-identity sources were excluded from this expansion.

Removed 409.66 MiB of retired Debug objects after validation; their shared
replacements occupy 136.23 MiB. The additional net reduction is **273.43 MiB**
(286,716,808 bytes), bringing both sharing passes to approximately **1.33 GiB**
of net Debug object savings and **1,019 fewer compilation entries**. Conservative
compile-contract partitioning limits the realized savings below the candidate
ceiling. Windows and Linux builds were not run locally.

The full Debug build, sharing fixture, three representative Release tests
(`difficulty_table_importer_tests`, `replay_file_action_service_tests`, and
`button_enabled_tests`), and incremental build checks passed. The parallel Debug
suite passed 402/404 cases; audio rendering and jukebox restoration exceeded their
existing 30-second limits. Jukebox restoration then passed twice in isolation
(25.99 and 19.78 seconds), and the final audio CTest rerun passed in 14.60 seconds.
A controlled comparison also confirmed that the audio executable is byte-identical
before and after this increment; direct baseline/current runs both passed in
36.46/27.25 seconds. No timeout policy or test fixture was changed. All 404 cases
passed across these runs; the single full parallel run is not reported as green.

## Inactive configuration prune (2026-09-24)

At the user's request, removed the inactive `cmake-build-lua-off`,
`cmake-build-lua-off-vcpkg`, `cmake-build-release-bundle`, and
`cmake-build-release-bundle-macos13` directories, plus 137 orphaned compiled
objects and test executables. Selection excluded tracked files, current Ninja
outputs and inputs, current CMake/Ninja/CTest references, dependency installations
inside retained trees, and the extraction benchmark workspace.

Removed artifacts occupied 2.135 GiB; observed available disk space increased
from 12.817 to 14.935 GiB. Remaining desktop Debug and Release trees occupy
7.675 and 0.403 GiB respectively. Both current Android configurations remain.
A full incremental Debug build passed afterward without compiling objects.
