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

## Device observations and validation limits

The first-play skin-readiness error and orientation loss were reproduced on the earlier build. After their fixes, three cold launches each entered gameplay on the first Start attempt. Short earlier Home/resume cycles preserved the process, but a longer final Home test reproduced the reported closure. Android exit-info recorded `LOW_MEMORY` for PID 20592 at 2026-10-07 11:10:33 KST; lmkd killed the background process with approximately 1.9 GB reported PSS/RSS. The crash buffer was empty. The user subsequently reported that the initial fix already appeared to resolve the crash and explicitly declined the proposed additional foreground service. No gameplay foreground service is included. Android can still reclaim the process under memory pressure; the observed OS termination is not evidence of a native crash.

Further service work and lifecycle stress testing were stopped at the user’s request. Existing automated regressions cover audio continuation, explicit pause, background judging, and deferred terminal handling; the longer device Home/recents/explicit-pause sequence did not complete because of the low-memory termination. Both final complete review rounds covered the retained implementation, with no production code changes afterward. No iOS platform rebuild or Firebase upload was performed for this follow-up.
