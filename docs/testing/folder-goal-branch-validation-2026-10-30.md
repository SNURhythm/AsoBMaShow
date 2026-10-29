# Folder-goal branch validation — October 30, 2026

## Release baseline

The latest successful TestFlight release is **0.0.1 (166)**, built from
`9ea2ad492a7da2c05fe5c3510a6dbb8c09a326e1` by
[workflow run 40046354059](https://github.com/SNURhythm/AsoBMaShow/actions/runs/40046354059).
The workflow records a successful upload and internal distribution on October 24
UTC. A read-only App Store Connect query confirmed build 166 as the newest upload,
with valid processing and no expiration, on October 30 KST.

That release uses score schema **14** and replay schema **19**. This branch uses
**15** and **21**. The validation includes changes inherited from `develop`,
particularly Light Assist normalization, as well as the goal feature itself.

## Findings and corrections

- Back from chart-specific recommendations skipped the recommendations overview.
  It now restores the ordinary overview and its saved filters and pagination.
- Switching a new EX rank draft through Lamp mode lost the exact rank fraction.
  The draft now retains its rank while Lamp mode remains a valid lamp target.
- Average EX and EX coverage targets shared one pending-type count. Each now has
  its own bubble row; three simultaneous target types fit a taller bubble within
  the existing safe-area bounds. Multiple unfinished goals of the same type
  still show a total count.
- Independent goals repeatedly copied and deduplicated the same folder evidence.
  Batch projection now groups targets by source and LN mode, aggregates once,
  and evaluates lamp histograms, an average, or a sorted exact-ratio score list.
  Aggregates last only for one score snapshot, with at most one scope aggregate
  held at a time. Before-play membership checks are also shared per source.
- The full suite exposed judgement-indicator color-range allocations in every
  captured frame. Presentation now shares immutable colorization snapshots;
  preview settings reuse unchanged snapshots, and picker edits replace them
  without changing already captured frames. The existing zero-allocation test
  now also exercises configured color ranges and retained-frame lifetimes.
- GOAL override selection was missing from visual authority equality. It now
  participates in equality so a changed override cannot compare as unchanged.

The optimization applies to overview loading and before/after-play evaluation.
It preserves original result ordering, missing/unplayed handling, LN isolation,
exact rank boundaries, fractional coverage rounding, and achievement-credit rules.
The existing evidence queries already read summary tables in two prepared scans;
there was no evidence justifying a persistent statement cache for those scans.

## Performance measurement

An optimized (`-O2`) standalone harness used 100,000 unique chart members and
100,000 score-summary rows, with 20 goals over one directory. Baseline and final
binaries ran sequentially after the native build had finished:

| Projection of all 20 goals | Before | After |
| --- | ---: | ---: |
| First call, including membership query | 510.8 ms | 110.9 ms |
| Reusing cached membership, pass 2 | 421.5 ms | 16.7 ms |
| Reusing cached membership, pass 3 | 423.5 ms | 17.7 ms |

The two score-summary scans took 32–37 ms separately in both versions; those are
not included in the projection times above. This measures CPU/SQLite projection,
not UI frame latency or an iOS device. The harness checks the resulting 50% EX
average. Automated tests also cover mixed targets/LN modes, result ordering,
changed evidence between batches, exact thresholds, and achievement credit.

## Migration evidence

The committed [release schema fixtures](../../tests/fixtures/testflight166/README.md)
come from the release's SQL builders/constants. Tests reconstruct those schemas
instead of changing only `user_version` on current tables.

Synthetic native regressions verify:

- Every existing score/course row and extension-table value survives. Only
  documented mutable lamp values normalize from 150 to 50; derived score caches
  may rebuild. Provenance JSON, result fingerprints, attempt IDs and links remain.
- Authenticated chart and course facts, stages, entries, replay-file references,
  snapshot JSON, IR outbox payloads, pending score writes, legacy summaries, and
  sequence values survive replay migration.
- Old pending chart writes recover with unknown gauge evidence, rather than
  invented values. Modern result fingerprints still validate after migration.
- A denied table creation after earlier migration writes rolls back both schema
  and data, leaving the old version intact. Reopening retries successfully.
- Repeated initialization is idempotent. The score test includes the old
  selector's persisted `ON score_db.scores` trigger.

The optional private-profile validation used disposable copies, first in the
fixture's actual **15/20** schemas, then reconstructed with the pinned **14/19**
release definitions and matching columns. Both upgrades preserved:

| Stored records | Count |
| --- | ---: |
| Chart scores | 833 |
| Course scores | 25 |
| Modern chart results | 326 |
| Modern course results | 12 |
| Replay-file references | 323 |

All existing table rows were compared, allowing only the explicit mutable lamp
normalization and derived summary rebuilds. Every modern chart/course result
loaded through the current repositories with a valid fingerprint. Integrity and
foreign-key checks passed; reopening left both databases unchanged. Existing
chart goals loaded successfully. Baseline database checksums stayed unchanged.
No private database or player record is included in the repository.

### Limits

The private fixture is a repaired numeric 15/20 profile, **not an untouched
export from build 166**. Reconstructing the release schemas exercises real record
content through the release-to-current path, but cannot establish the exact state
of every device. Settings, credentials, and replay payload files are absent from
this fixture. Their persistence is covered by the repository's synthetic profile
and release tests, not by this private-data check or an on-device upgrade.

The replay files referenced by this fixture are intentionally missing, so this
check validates reference preservation and result loading, not replay playback.
No distribution, deployment, merge, or change to the private baseline was performed.

## Final verification

- The full desktop build passed with `cmake --build cmake-build-debug -j 6`.
- The final full CTest run passed **545/546** tests in 74.75 seconds. The remaining
  `find_bms_ios_file_bridge_tests` failed its user-controlled retry/cancel checks,
  then passed alone in 5.62 seconds. It also passed in the earlier full run;
  its implementation and fixture are unchanged from `develop`. This is an
  observed intermittent failure with no confirmed root cause, not a clean
  all-green full-suite claim.
- The initial full run exposed the frame-allocation regression and an in-memory
  setup error in the new migration test. Both were corrected and passed the
  final full run, including the additional GOAL equality regression.
- `IOS_RELEASE_BUILD_JOBS=6 scripts/ios_release_verify.sh` passed: 68 native
  release-critical tests, 55 build-setup tests, 20 workflow tests, 18 artifact-audit
  tests, the unsigned generic-device iOS build, and the built-app artifact audit.
  Signing was intentionally skipped. No archive or upload was performed.
- `git diff --check` passed. Native visual inspection and a physical-device
  upgrade remain outside this verification.
