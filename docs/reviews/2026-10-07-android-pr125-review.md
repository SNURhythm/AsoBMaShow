# PR #125 review and Android lifecycle follow-up

Review base: `0d16c9cc83fd314d12f6c7087f363d883f4a2ff1` (branch merge base with develop).
The review covers the entire branch diff, not only the follow-up fixes. Three independent read-only partitions cover Android Java/platform tests, native library/skin/UI/build/docs, and gameplay/audio/rendering/input. Each round must complete all partitions; any actionable finding resets the consecutive-clean count.

## Requested behavior

- Resolve actionable PR feedback and obtain two consecutive complete reviews with no actionable findings.
- Active Android gameplay continues audio and chart judging while backgrounded. Explicit pause remains paused; ordinary non-gameplay scene audio suspends. Music Mode retains its separate native background path.
- Startup skin preparation uses current viewport orientation. Returning to gameplay restores fixed orientation and framebuffer bindings.
- Folder Move verifies source contents and metadata, reserves its output, and persists files and directory ancestry before incremental source deletion. Cancellation must reach blocked provider discovery and verification.

## Existing PR feedback

| Review comment | Resolution |
| --- | --- |
| 4197818484 | Reject Move without reliable size and modification metadata; Copy remains available. |
| 4197818489 | Update host stubs for document initialization. |
| 4199027791 | Keep database/profile trees readable and immutable through Files. |
| 4199027802 | Reserve Move destinations against Files mutations through cleanup and partial completion. |
| 4199027810 | Cancel callback-owned touches and drags at lifecycle boundaries. |
| 4199964739 | Re-read source bytes and compare SHA-256/length before deletion, then revalidate metadata without a pause window. |
| 4200440773 | Remove unsupported database replacement instructions. |
| 4200440780 | Drain Android deferred touch callbacks before realtime input commands. |
| 4200440786 | Preserve Add Folder in the optional all-files menu. |
| 4201982885 | Refresh authored Android touch geometry while realtime authority is active. |
| 4201982889 | Do not persist one-shot import picker grants; preserve durable Add Folder grants. |
| 4201982893 | Keep Android credential-cleanup journals private while retaining adjacent atomic profile-overwrite markers. |

## Whole-branch review rounds

| Round | Android | Native library/skin/UI/build/docs | Gameplay/audio/rendering/input | Result |
| --- | --- | --- | --- | --- |
| 1 | Two findings | Clean | Two findings | Reset clean count to zero. |
| 2 | Clean | Clean | One finding | Reset clean count to zero. |
| 3 | Clean | Clean | Clean | First consecutive clean round. |
| 4 | Clean | Clean | Clean | Second consecutive clean round. |

Round 1 fixes:

- Sync destination directories and their ancestor path before deleting any source subtree, including empty folders. Fail closed on barrier errors.
- Independently monitor cancellation during blocked chart-provider root queries, discovery, and final revalidation; cancel provider operations, close tracked streams, and drain writers before cleanup.
- Discard Android touches on both realtime pause and resume so overlay taps and held ownership cannot reach gameplay afterward.
- Give legacy survival failure priority over completed timelines and practice looping when foreground processing resumes, preserving recorded-abort replay semantics.

Round 2 finding: background survival failure must stop subsequent judgements and replay events within the same tick. A two-note Hard-gauge reproduction produced two events in background versus one in foreground. A separate deferred terminal boundary now stops scoring and replay/HCN/mine/timeline work until foreground result handling. Actual production judging/timeline regressions passed red → green, including replay-abort exemption and new-attempt reset.

## Verification

- Desktop full build passed. Full CTest run passed **444/444 tests** in 381.33 seconds with `-j 6`.
- The two Round 1 gameplay fixes subsequently passed the full standalone terminal fixture and five focused CTests (terminal scene, legacy touch, audio lifecycle, Android render suspend, display orientation).
- Java host coverage includes **40 chart import, 8 worker, and 4 copy-control tests**, plus the skin import suite. Blocked query cancellation and missing durability barriers were demonstrated before their fixes.
- Android-specific private credential-cleanup tests passed a standalone build with `TARGET_OS_ANDROID=1`; the normal desktop target does not execute those guarded assertions.
- Final desktop main/terminal-fixture rebuild and all five focused CTests passed after the Round 2 fix. The final signed restricted-file-access Android build also passed (55 seconds); it includes all gameplay terminal fixes. Three subsequent device cold starts each entered gameplay on the first Start attempt.
- On the connected Galaxy S20 FE / Android 13, import-cancellation, DocumentsProvider, and skin-directory instrumentation passed. This covers real SAF cancellation during root/list/final revalidation and directory sync in actual app-private Files and Documents, including empty directories and parents.
- An earlier standalone Android Java probe also confirmed `FileChannel.open(directory, READ).force(true)` support on internal and emulated external storage. These checks establish API/filesystem support, not behavior during physical power loss.
- The first instrumentation run exposed a test-fixture latch shadowing error. Renaming the local latches made the intended assertion observe the provider callback; production code did not change for that correction.

## Follow-up after merging develop

The branch merged develop at `6a2fb4ade0b0ef02bba99f64f49a45f7d76a2d1c`. Merge verification passed the desktop app and affected test builds, 14 focused CTests, and 51 iOS build-setup host tests. The merge preserves lane-specific judgement and Chinese localization alongside Android lifecycle fixes.

New PR comment **4202455069** identified case-sensitive BMS dirty tracking. `documents:bms` and mixed-case aliases may name the same library on case-folding storage. Dirty tracking now compares the first relative path component case-insensitively, preserving exact root IDs and component boundaries. The policy regression and actual provider mutation regression both failed before the fix. The latter checks persisted dirty state, writer-close deferral, refresh scheduling, and acknowledgement for create/write/rename/delete across `BMS`, `bms`, and `BmS`.

The user requested another whole-branch review loop. Each round reviews the entire branch against the merged develop base, including uncommitted follow-up changes; the clean count starts again at zero. Rounds 5–6 covered 149 files. Round 7 includes the Activity regression harness (151 files); the native storage-path regression brings Round 8 coverage to 152 files. Round 9 includes touch timestamp propagation and its regression coverage (157 files). Rounds 10–11 include direct realtime ingress and its native session regressions (160 files). Rounds 12–13 include shared synthetic-pointer filtering (161 files).

| Round | Android | Native library/skin/UI/build/docs | Gameplay/audio/rendering/input | Result |
| --- | --- | --- | --- | --- |
| 5 | Two findings | Clean | Clean | Reset clean count to zero. |
| 6 | One finding | Clean | Clean | Reset clean count to zero. |
| 7 | Clean | One finding | Clean | Reset clean count to zero. |
| 8 | Clean | Clean | One finding | Reset clean count to zero. |
| 9 | Clean | Clean | One finding | Reset clean count to zero. |
| 10 | Clean | Clean | Clean | First consecutive clean round. |
| 11 | Clean | Clean | One finding | Reset clean count to zero. |
| 12 | Clean | Clean | Clean | First consecutive clean round. |
| 13 | Clean | Clean | Clean | Second consecutive clean round. |

Round 5 found two additional provider boundary cases, both reproduced with the production provider in the host harness:

- A writable file opened outside BMS could remain open while its parent was renamed into BMS. Dirty tracking now counts all writable Documents handles, so the rename-triggered refresh waits until the last writer closes. Closing restarts debounce only when a refresh is pending; unrelated writes alone do not dirty the library.
- Internal storage fallback could use an aliased ancestor path such as `/data/user/0` while the path policy stored its canonical root. Initialization now resolves the root through the policy before creating Skins, retaining rejection of symlinked document entries.

Both regression tests failed before their fixes and passed afterward. Six related Android host CTests passed, covering provider boundaries/mutability, destination reservations, chart/skin import, and temporary storage. The provider host suite includes 13 tests, including overlapping writers and canonical fallback CRUD. Round 6 found that Activity callers still passed raw aliased Documents paths into the canonical policy. The shared BMS helper and Copy/Move setup now resolve their roots through the policy too. Extracted production Activity methods reproduce Open in Files and import behavior against the real policy, provider, and copy engine: three tests passed after failing before the fix. They verify internal fallback and external aliases, canonical document IDs, copied bytes, Move-only deletion, and rejection of a symlinked BMS directory. Eight registered Android host CTests, the desktop main build, and the signed Android build subsequently passed.

Round 7 found a remaining external-container alias compatibility gap in native skin storage. SDL canonicalizes its internal storage getter but returns an absolute external path that can retain aliases. The native external-root bridge now canonicalizes only that trusted container before appending Documents; no-follow checks still reject symlinked Documents or Skins entries. This edge case predates the base and is addressed as part of consistent external Documents/skin support, not claimed as a newly introduced regression or an observed device failure. An extracted production bridge → Documents path → skin root → no-follow regression failed before the fix and passed afterward, including descendant symlink rejection and existing fallback behavior. The desktop main build, nine related Android host CTests, and signed Android build passed afterward (29-second Android build).

Round 8 found that deferred Android touch callbacks discard the event timestamp. The logical pipeline substitutes queue-drain time, which can shift judgement and replay timing and collapse the spacing between batched press/release events. The source now captures steady-clock time at watcher ingress and stores it with each queued event. A scoped callback timestamp reaches physical touch lane transitions, judgement/replay observers, and scene touch samples mapped through the audio clock. The delayed-drain source regression failed before the fix; it now passes with distinct original press/move/release times. Logical pipeline tests verify judgement and replay edge timestamps, and extracted production handler/scene methods verify touch-sample clock mapping. The desktop main build and 17 focused CTests passed afterward (7.04 seconds). The signed restricted-file-access Android build also passed (1 minute 17 seconds). Round 9 subsequently identified the separate worker-admission issue below.

Round 9 found that timestamp preservation alone cannot reverse an automatic miss already committed while an Android touch waits for the rendering thread. Android realtime authority now routes SDL ingress directly through the existing realtime touch router with immutable published geometry; only auxiliary presentation/replay samples wait for the main thread. Legacy fallback retains deferred callbacks with ingress timestamps. Source callback replacement joins active SDL watchers before session teardown, and pause/background/overflow gates cancel router ownership. An SDL-source → native router → real-worker test reproduced the missing pre-drain judgement before the change and passed after it. Native session ingress/publication methods are also extracted for host testing of disabled, background and interrupted admission. The first expanded lifecycle run caught a held-key release ordering regression: disconnecting physical devices must precede closing admission. After correcting that order, the desktop app and native fixtures rebuilt, all 18 focused CTests passed (6.87 seconds), and the signed restricted-file-access Android build passed (48 seconds). Round 10 was clean; Round 11 found the compatibility issue below.

Round 11 found a retained pointer-identity compatibility gap: SDL sends touch-generated mouse events before their native finger event by default, and mouse-to-finger ID 0 could collide with a real Android pointer. This was also unsafe in the earlier legacy path; it is not claimed as a new regression or device-observed failure. Gameplay ingress now filters both synthetic mouse/touch duplicates and gives real mouse emulation an identity outside Android's nonnegative pointer IDs. Source tests cover both legacy and raw dispatch, while a real-worker regression reproduces synthetic mouse → touch 5 → touch 0 and verifies independent note hits and releases. Both failed before the correction. The desktop app rebuilt and all 18 focused CTests then passed (6.25 seconds). The signed restricted-file-access Android build also passed (2 minutes 21 seconds). Rounds 12 and 13 each reviewed all 161 changed files with rotated reviewer assignments and found no actionable issues. No production code changed after those verdicts.

## Device observations and validation limits

The first-play skin-readiness error and orientation loss were reproduced on the earlier build. After their fixes, three cold launches each entered gameplay on the first Start attempt. Short earlier Home/resume cycles preserved the process, but a longer final Home test reproduced the reported closure. Android exit-info recorded `LOW_MEMORY` for PID 20592 at 2026-10-07 11:10:33 KST; lmkd killed the background process with approximately 1.9 GB reported PSS/RSS. The crash buffer was empty. The user subsequently reported that the initial fix already appeared to resolve the crash and explicitly declined the proposed additional foreground service. No gameplay foreground service is included. Android can still reclaim the process under memory pressure; the observed OS termination is not evidence of a native crash.

Further service work and lifecycle stress testing were stopped at the user’s request. Existing automated regressions cover audio continuation, explicit pause, background judging, and deferred terminal handling; the longer device Home/recents/explicit-pause sequence did not complete because of the low-memory termination. The earlier clean rounds predate the follow-up fixes above. The final follow-up rounds (12 and 13) both cover the retained implementation, with no production changes afterward. No iOS platform rebuild or Firebase upload was performed for this follow-up.
