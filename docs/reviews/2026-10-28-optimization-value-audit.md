# Optimization value audit

Scope: this branch's changes from `origin/develop` through `921a82d5`, followed
by this cleanup. The acceptance criterion is less actual work or a justified
scheduling/correctness contract, not a smaller isolated delay number. No physical
input-to-photon or input-to-audio improvement has been established.

## Removed experiments

- **Deliberate later preparation:** already removed in `921a82d5`. Capture 24
  reduced preparation-to-display age by about 3.8 ms but added deadline misses.
  The native gameplay input/judgement/keysound worker is independent of rendering;
  the preparation-age metric does not measure that path or input-to-photon latency.
  The log 23/24 recommendations are now explicitly withdrawn.
- **Two-slot timer-fallback presentation gate:** deleted, including its scoped
  predicates, native callbacks, transition cancellation and dedicated fixtures.
  It waited before a third drawable acquisition until an older drawable was
  reported displayed (up to 50 ms). Scene/input preparation had already happened.
  It removed no rendering work; the reported ~15.9 ms versus ~16 ms Present Delay
  did not isolate a benefit under matched conditions. CAMetalDisplayLink already
  bypassed it. Ordinary fallback now uses native drawable acquisition and the
  existing frame-budget timer without another presentation-completion limiter.
- **Offscreen-before-clear view ordering:** restored clear → BGA/layer → blur →
  final composition → gameplay → UI → readback. The experiment moved drawable
  acquisition later while submitting the same rendering work. This can change
  measured drawable residence/wait placement, and could affect overlap, but no
  controlled throughput or end-to-end latency benefit was established. Do not
  claim the restoration itself improves performance.

The earlier one-frame GPU limit and strict end-of-loop presentation wait were
already reversed. The latter had reduced observed cadence to 60 FPS.

## Retained

- **CAMetalDisplayLink**, explicitly requested by the user, including drawable
  ownership, lifecycle/export handoff, fallback, rate selection, preferred frame
  latency one, and correlated timing diagnostics. This is the selected platform
  pacing mechanism; its retention is not a claim of measured latency improvement.
- **Three Metal drawables and GPU limit three:** normal overlap/headroom, with
  the unsuccessful single-GPU-frame limit removed. These are distinct from the
  deleted two-slot gate.
- **Frame-budget accounting:** acquisition/submission consume the frame budget;
  they do not start an additional full timer interval. Existing deterministic
  regressions demonstrate prevention of double waiting. Idle cadence and
  explicit caps remain covered.
- **BGA/blur work reductions:** zero-blur bypass, eligible direct output, unchanged
  fullscreen-content reuse and redundant-clear removal eliminate passes/transfers.
  Production-path Mac Metal probes show fewer submissions and pixel-equivalent
  cached output. Direct output can be sharper without the old half-resolution
  intermediate; fewer passes do not guarantee lower GPU cost in every case.
- **Allocation and gameplay scaling reductions:** borrowed cache lookup keys,
  reused submission scratch, sorted timeline membership, bounded HCN/PMS
  traversal and immutable shared pacemaker progressions remove measured allocation,
  copying or future-chart scans. Ordinary-chart savings may be small.
- **Library/UI/persistence reductions:** narrow record lookup, category-existence
  caching, unchanged-upload reuse, retained-row motion and bounded text residency
  reuse avoid specific repeated work. SQL can still scan/sort; recycler motion
  still traverses descendants and invokes Yoga; upload caching has comparison
  costs. Rate-slider persistence coalesces many saves into one, but the remaining
  synchronous completion save can still block and a crash mid-gesture can lose
  the unsaved final value. These costs were not eliminated or hidden in claims.
- **Correctness fixes:** current-frame submission acceptance/recovery, transient
  allocation checks, export limits and clamped blur edges remain. Acceptance is
  not GPU completion or successful display.

The independent read-only audit of retained changes found no additional removal
justified. It also caught a stale description of permanent cache invalidation;
the gameplay-rendering report now describes the implemented per-frame recovery.
Historical experiment plans remain, with current decisions marked explicitly.

## Verification

The generated-Metal regression first failed with the gate present, then passed
with its hooks removed while requiring the display-link provider/timing hooks.
Desktop `main`, rendering/video/iOS test targets and the unsigned iOS device
build passed. Three affected iOS build-setup tests passed.

The combined 92-test run passed 91 checks; the BGA Metal test still asserted the
removed experimental view order. Its assertions were updated to require the
restored dependency order while retaining draw counts and pixel checks. The
focused rerun passed, completing all 92 selected checks. No deployment or device
benchmark was performed for this cleanup.
