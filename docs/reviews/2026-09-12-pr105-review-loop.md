# PR 105 whole-branch review loop

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
- Whole-branch reviewer/fixer rounds are in progress after the external fixes.

No deployment is authorized or performed. macOS Foundation fixtures are not
physical-iOS runtime evidence. Application-retained metadata bytes are bounded;
Foundation's internal network buffers remain outside that admission boundary.
