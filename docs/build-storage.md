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
without native compilation. Older Android configurations and their four local
dependency installations remain on disk; this change prevents further compatible
configuration copies but does not claim those retained directories were reclaimed.

The full desktop build and two representative Release tests passed. Both new
build-configuration tests passed. Two full CTest runs at `-j 6` passed 403 of 404
cases; `chart_audio_renderer_tests` exceeded its existing 30-second timeout in
both parallel runs, passed in isolation in 12.94 seconds, and passed three more consecutive
isolated runs in 12.92–13.09 seconds. Its large audio mixing workload remains sensitive to concurrent load; no test timeout or fixture
was changed for this storage work.
