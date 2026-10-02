# PR 115 branch review — 2026-10-02

Review base: `deb6b394da2e8ff8bd330df273bb1a6f85d7f086` (`develop`). Initial
branch head: `01e40867bf38bc6b5daf89c14daad0afda198f66`. The complete branch
diff contained 171 files. The baseline desktop build and all 410 tests passed
before this review.

## Review coverage

Five independent review assignments used `gpt-6.1-sol`: medium reasoning for
onboarding/platform and build/reference tooling, high reasoning for Lua/property
semantics, rendering/input, and session/settings/result lifecycle. Each reviewer
received a scoped diff and explicit requirements without inheriting the prior
implementation discussion. Root owned reproduction, the build directory, and
integration. Separate reviewers checked the fixes.

Reviewed areas included first-run tables/tutorial/persistence, desktop folder
actions, Lua and JSON decoding, host coercions and state properties, rendering
and retained blend state, pointer transformations, skin mode settings and
package lifecycle, course results and exports, build integration, and historical
reference/artifact provenance.

## Findings and corrections

| Severity | Finding | Correction and regression |
| --- | --- | --- |
| P2 | Quitting while the new desktop folder chooser is open joins an uncancellable native call and blocks application teardown. | The desktop worker owns only shared result state and a copied title. Teardown requests cancellation and detaches; live service polling performs repository/task actions. A blocked-dialog regression failed before the fix and now checks prompt destruction and discarded late selection. |
| P2 | JSON callback setup turns fatal Lua quota/resource failures into warnings and fallback bindings, allowing a partial Standard-mode skin to activate. | Reuse the existing fatal binding classification, reject the model, and stop subsequent script compilation. Decoder tests cover fatal classes and recoverable syntax errors; an actual Standard runtime infinite factory reproduces quota exhaustion during session activation. |
| P2 | A transparent nested song-list level emits numeric commands that incorrectly change the retained blend used by following font text. | Skip only numeric draw lowering after normal preparation completes, matching the reference alpha-zero draw path. Regressions cover scalable/bitmap fonts and the following frame’s retained blend. |

Fix review caught a pending-result race in the first picker implementation:
selection A could arrive after polling and be overwritten by selection B.
The final sequence admits a new request, joins the completed prior worker,
drains its result, then starts the new worker. A repeated-selection regression
checks that both folders are registered and each queues exactly one scan.

The initial focused regressions failed for the expected reasons: one shutdown
assertion, six renderer assertions, eight JSON decoder assertions, and two
actual session activation assertions. All four affected test targets then
passed. Independent fix reviews reported no remaining actionable issue.

## Verification and limits

`cmake --build cmake-build-debug -j 6` passed.
`ctest --test-dir cmake-build-debug --output-on-failure -j 6` passed all 410
tests in 74.98 seconds, including the final repeated-selection regression and
actual Metal rendering checks. `git diff --check` passed.

A final real-SQLite writer-contention probe did not reproduce a UI polling
stall; the read-before-write transaction upgrade returned promptly. Review
found no concrete latency regression, so that concern did not trigger an
additional architectural change.

Review preserved the documented corrected subtractive blend and explicit
per-command sampler behavior. Historical oracle pins remain intentional; their
tools read exact Git snapshots while the current reference stays at `ad42f56c`.
Native picker shutdown was tested through the existing blocking dialog stub;
this pass did not run a real native dialog or mobile device build. Live source
availability for bundled table snapshots and randomized number-format parity
were not independently tested. A clean review covers the inspected paths, not
every possible external skin or runtime environment.

## Continued review loop

The next full pass started at `db3180c1ea13bf9113e0ec0730cbd19ce4ec35dc`.
Fresh reviewers used medium reasoning for onboarding/platform/tooling and high
reasoning for Lua semantics, rendering, and lifecycle/results. It found two
additional issues:

| Severity | Finding | Correction and evidence |
| --- | --- | --- |
| P1 | The new Lua float formatter used the compiler-specific `__uint128_t`, blocking the repository's MSVC Windows builds. | Replace it with four portable 32-bit limbs and only the bounded operations needed for decimal digit generation. A header compile rejecting native 128-bit extensions failed before the fix and passed afterward. Boundary regressions exercise subnormals, powers of two, wide arithmetic and both signs. |
| P2 | Repeated destinations or image-set references to one JSON image recompiled its timer factory, creating distinct callback state for one authored property. | Cache compilation results by authored JSON field and binding kind. Repeated references share the callback while separate fields with identical scripts stay independent. The new regression failed two assertions before the fix; it also checks repeated image actions and text values. |

The formatter replacement uses the standard integer widths documented by
[Microsoft](https://learn.microsoft.com/en-us/cpp/cpp/int8-int16-int32-int64?view=msvc-170).
A direct C++ comparison against Java 8 `Float.toString` matched all 5,627 cases:
exponent boundaries and neighbors, signed values, special values, and 4,096
seeded random bit patterns. Independent arithmetic review also checked digit,
shift, carry and capacity bounds. Actual MSVC execution is unavailable on this
macOS machine; the extension-rejection probe is a portability check, not a
Windows application build.

Independent JSON fix review verified that cache keys refer to stable nodes in
the parsed document, separate authored fields remain independent, and fatal
compilation failures still reject the model. The affected JSON decoder and
renderer tests passed together in 1.59 seconds.

A subsequent complete fresh review pass covered the updated branch with three
new reviewers: medium reasoning for onboarding/platform/build tooling and high
reasoning for Lua/lifecycle/settings and rendering/results/exports. All three
reported no actionable introduced findings. This is the loop's clean stopping
pass, following the two additional fixes above.

The final all-target desktop build passed, followed by all 410 CTest tests in
70.33 seconds. `git diff --check` passed. No Windows/mobile application build,
deployment, or merge was performed in this continuation.
