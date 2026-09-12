# PR 105 whole-branch review loop

Status: **complete — all three reviewers report CLEAN in round 3**. The three
external findings and two additional cache findings are corrected. This is a
converged review result, not a guarantee that the branch is defect-free.

## Scope

Address the three new external findings at `d1f9e0be`, then repeat independent
reviewer/fixer rounds over the entire branch diff against the `develop` merge
base `de1c8d45d4fdf50429bc2fbe40b9d990859be7b1`. Reviews are not restricted to
the newest corrections. Only concrete actionable defects extend the loop;
style preferences and speculative refactors do not.

## External findings

- Mixed encrypted/unencrypted 7-Zip indexes now retain their incomplete coverage
  as encryption metadata. Full unzip rejects them before destination creation,
  completed-folder reuse, or source deletion; ordinary unencrypted-member
  browsing remains available. Disk index version 5 preserves the flag and
  rebuilds older caches. Genuine mixed-encryption fixtures cover cold, memory,
  disk and legacy caches and Delete Originals source preservation.
- Full unzip writes, closes and checks its incomplete ownership marker before
  the first post-directory cancellation checkpoint. Exclusive directory
  creation remains required. Deterministic journal-callback and post-directory
  cancellation tests exercise actual recovery cleanup and successful retry.
- iOS GET and POST metadata callers propagate their response caps into a shared
  data delegate. Known lengths and streamed chunks are admitted before retained
  accumulation. Cancellation, redirect restrictions, UTF-8 validation and
  existing callers are preserved. The default cap is 16 MiB; zero admits only
  an empty response. Successful publication preserves embedded NUL bytes.

Both correction sets received independent scoped review with no actionable
findings. Eight archive regressions failed before the fixes and pass afterward.
The Foundation transport regressions likewise reproduced the missing caps and
exercise exact/over/zero limits, unknown/chunked responses, redirects, stalled
responses, cancellation, UTF-8 and publication behavior.

## Validation and loop status

- Both archive suites pass three consecutive runs each.
- The native Foundation bridge suite passes three consecutive runs; the two
  related release-contract checks pass.
- The all-target desktop build and parallel CTest pass: **357/357** tests.
- iOS release verification passes: 66 native suites, 88 Python contract checks,
  unsigned arm64 device build and artifact audit.
- Android Firebase release build-only verification passes, including release
  lint and APK packaging. No upload is performed.
- Three whole-branch rounds are complete; the final three reviewers report no
  actionable findings.

No deployment is authorized or performed. macOS Foundation fixtures are not
physical-iOS runtime evidence. Application-retained metadata bytes are bounded;
Foundation's internal network buffers remain outside that admission boundary.

## Whole-branch round 1

Three independent reviewers surveyed the complete 206-file branch diff and all
changed production domains. Correctness and lifecycle/performance passes found
no additional actionable defects. Security/platform review identified one P2:
unchecked persisted member paths could make implicit-parent enumeration loop
forever at the filesystem root, ignoring cancellation. Traversal paths could
also create reservation directories outside the owned unzip destination.

The fixer reproduced the root hang under a bounded watchdog and the outside
reservation write with an otherwise valid cache and unchanged archive identity.
Deserialization now rejects and rebuilds unsafe/noncanonical member paths; full
unzip independently checks the path invariant before accounting or reservation
work. Ancestor walks also observe cancellation. Seven malformed-cache cases
and a valid nested/Unicode-name control pass, as do both focused archive suites.

Combined validation of this correction passes: all-target desktop build,
357/357 CTest, iOS release verification and Android release build-only. Reviews
inspect production/build changes across domains; test helpers and embedded
fixture bytes receive selective inspection, not exhaustive line/byte auditing.

## Whole-branch round 2

Three fresh reviewers again inspected the entire branch. All accepted the path
correction. One identified another P2 in the same cache trust boundary: changing
the persisted encryption flag could restore plaintext-only extraction and make
the original archive eligible for deletion. The other passes reported no
additional findings; consolidation agreed that the combined round is not clean.

Full unzip now requires an immutable manifest derived from the live source.
Disk-restored metadata, including memory hits originating from disk, cannot
grant that authority. A private in-memory provenance bit is never serialized;
promotion uses the existing coalesced builder and cancellation/entry-limit
checks. Normal browsing still uses disk caches. The first full unzip after
disk-backed browsing may enumerate the source once more; subsequent operations
reuse the same-source live manifest.

Eight regressions reproduce and then reject forged encryption flags, real
completed-folder reuse, actual original deletion, omitted/renamed members,
forged entry counts and incorrect live-promotion coordination. Both focused
archive suites pass.

## Whole-branch round 3 and final verification

Three fresh reviewers independently inspected the entire 206-file branch diff,
with correctness, security/platform and lifecycle/performance emphasis. Every
pass covered all changed production/build/platform domains, followed integration
callers and reassessed both cache corrections. All three report **CLEAN**.
No confirmed actionable finding remains from the review loop.

| Round | Reviewers | Combined result |
| --- | --- | --- |
| 1 | 3 whole-branch passes | One P2: persisted path validation; corrected |
| 2 | 3 whole-branch passes | One P2: persisted manifest authority; corrected |
| 3 | 3 whole-branch passes | No actionable findings |

Final source/test snapshot validation on 2026-09-12:

- All-target desktop build passes; parallel CTest passes **357/357** tests.
- iOS release verification passes **66 native suites**, **88 Python contract
  checks**, unsigned arm64 device build and artifact audit.
- Android Firebase release build-only passes, including release lint and APK
  packaging. No distribution action is run.
- `git diff --check` passes. Unrelated local review inputs remain untouched.

Reviewers performed static inspection, not independent runtime test execution.
Test inventories and build integration were covered throughout, with selective
inspection of helpers and embedded fixtures rather than exhaustive byte audits.
No Windows-native build, physical-device run, sanitizer campaign, new performance
benchmark or power-loss durability certification is claimed for this loop.
