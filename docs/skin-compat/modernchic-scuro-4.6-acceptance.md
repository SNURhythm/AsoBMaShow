# ModernChic/SCURO 4.6 acceptance record

Status: **pending**. Task 1 pins the source/package contract; milestone closure
requires every schema-v1 completion criterion to be `pass` with external
evidence.

## Official package provenance

| Field | Pinned value |
| --- | --- |
| Product/version | ModernChicPlay (SCURO) 4.6 |
| Official release page | <https://www.kasacontent.com/musicgame/beatoraja/6088/> |
| Official download | The release page's linked ModernChic 4.6 download |
| Exact archive filename | `ModernChic460.zip` |
| Archive bytes | `453707440` |
| Archive SHA-256 | `3e36eef69d2f5f3b117696e22936347d9c8f5e379f8912d763a074ca9bcbbe4c` |
| Acquisition/access date | 2026-08-09 |
| Canonical archive package prefix | `ModernChic` |
| Corresponding extracted root identity | `skin-tree:515d52ba1a00a6067f636f838c935328b7765aef8f0019557748a2441ebb89df` |
| Archive payload `SkinTreeDigestV1` | `515d52ba1a00a6067f636f838c935328b7765aef8f0019557748a2441ebb89df` |
| Extracted source `SkinTreeDigestV1` | `515d52ba1a00a6067f636f838c935328b7765aef8f0019557748a2441ebb89df` |
| Selected 7-key entry | `play7_hw.luaskin` |
| Selected entry identity | `entry-d5399e62255ddbda273e7a63` |
| Selected entry SHA-256 | `aac73c59526e74f159731608b3b54e58a7e534537503acb88565a4c175cf8f13` |
| Selected Lua closure `SelectedLuaClosureContractV1` SHA-256 | `9d2a5acafc57edd6fa958c86fd92b861b851d952581a6828c1beb5b7fde67c0d` |

The archive and extraction live outside the repository. The equal, independently
computed tree digests bind the audited source tree to the exact official ZIP.
The repository contains only hashes, opaque inventory IDs, public provenance,
and the selected entry identity.

## Author terms and screenshot decision

The authoritative packaged readme inside the hashed archive permits use of the
included data, prohibits redistribution of the skin itself without permission,
requests KASAKO attribution for published modifications, and disclaims
responsibility for modified skins. The exact 4.6 release page above was
accessed on 2026-08-09; it identifies the package/version and linked download,
but is not used as a substitute for the packaged-readme terms.

Decision for this project:

- local compatibility testing is permitted;
- private, access-controlled gameplay screenshots are permitted;
- the package, extracted sources, assets, and public physical-evidence URLs
  must not be committed or redistributed; and
- any later public screenshot publication requires a separate review of the
  captured third-party content and attribution.

## Frozen acceptance schema version 1

The machine-readable source of truth is
`tests/fixtures/beatoraja_skin/reference_manifest.json`. It freezes these
requirements before renderer work:

- a non-unique iPad hardware model identifier, exact iPadOS version, drawable
  width/height, safe insets, and actual configured refresh rate;
- a clean measurement build commit/configuration;
- exact external archive, payload-tree, selected-entry, configuration, and
  activated-revision digests, plus the exact selected Lua closure digest over
  every loaded virtual identity and exact source byte, with configuration and
  activated-revision values explicitly pending until physical evidence exists;
- exact checked-in digests for the five redistributable synthetic BMS/BGA
  fixture files, independently of the still-pending scenario bindings; and
- still-pending synthetic chart hashes and fixed autoplay-script hashes for
  normal notes, every supported LN/CN/HCN phase, BPM/stop/scroll changes,
  chords, all judgment grades, combo breaks, gauge thresholds/failure, lane
  cover, BGA transitions, and song end;
- screenshot timestamps and a per-layout external evidence reference, without
  image bytes in Git;
- an explicit proof that the selected configured model has zero custom-timer
  and custom-event map entries, while synthetic evidence documents the pinned
  `IntMap` RNG-dependent order and AsoBMaShow's deterministic authored-order
  divergence for future nonempty maps;
- an opaque aggregate of every selected `dofile`/`io.open` mode, handle method,
  load/render reachability, and configuration guard without external paths or
  option labels;
- a frozen session-critical negative render-I/O scenario with exact
  diagnostic/fallback, a before overlay digest completed asynchronously before
  chart/session binding, an after digest computed only after session teardown,
  required digest equality, memory-only timed-path polling, the selected
  denied-operation kind, and expected/observed canonical opaque guard-vector
  digests for both negative and passing configurations;
- all six 16:9 and 4:3 Fit/Stretch/Custom layout cases;
- a 30-second warm-up followed by three complete 180-second repetitions for
  every scenario/layout; and
- `pending|pass|fail` plus an evidence reference for every completion criterion.

The limits are p99 skin CPU time at or below 90% of the actual refresh interval,
missed presentations at or below 0.5%, zero performed and zero denied
active-render filesystem reads, writes, directory scans, or resource uploads
for every passing run, no live texture/resource growth after ten completed
exits, and no more than 32 MiB resident-memory drift after warm-up. The negative
probe still requires every performed counter to remain zero and only its frozen
denied-operation counter to become nonzero.

The passing audit vector is
`38fe362a92f6ca0109effaa706d1ec42d6b08d28a394db738d2bccd8f63f7450`;
both opaque render-I/O guards are `not-reachable`. The negative audit vector is
`08d59b63dc1ed53154ac07baa9d5c72d2a0111ea8a5fd451f8ebc149537009bb`;
exactly one guard is `reachable`, its first post-transition attempt is the
frozen `filesystemRead` kind, and the other guard remains `not-reachable`.
These values are static, domain-separated evidence over the selected revision,
entry, effective opaque runtime option/choice selections, and guard outcomes.
They do not replace the still-pending physical `SkinConfigurationDigestV1`
value.

The selected-closure digest is an input gate, not evidence inferred from the
Lua scanner. It is domain-separated and canonically ordered; only its digest is
stored. All configured-model and retained-operation evidence is conditional on
an exact match. Any closure byte/path/add/remove change requires explicit
manifest and acceptance review. The scanner tests characterize
this pinned SCURO 4.6 closure and do not claim general Lua verification.

The negative run must deny the read before effect, emit
`skin_file_render_phase_denied`, discard that frame, disable the skin session,
and present the initialized built-in renderer in the same frame. Its performed
read/write/directory-scan/resource-upload counters remain zero; only the denied
read counter becomes positive. The before overlay digest must complete
asynchronously before chart/session binding; the after digest is computed only
after session teardown, and the two digests must be equal. Timed-path polling
is memory-only over precomputed status. Both digest values remain pending until
the physical run is recorded.

Task 1 permits `pending`. Final acceptance permits only `pass`.

The five checked-in BMS/BGA fixture digests establish redistributable input
provenance only. They neither bind the static chart to any of the four
acceptance scenarios nor substitute for autoplay, controlled-miss/retry, or
physical iPad evidence. Those scenario hashes and all device-derived values
remain pending until recorded by the real acceptance run.

### External physical-evidence metadata

The committed contract's `physicalEvidence` record intentionally remains
`pending` until a real device run. Its only external input is the bounded,
metadata-only `acceptance-evidence.json` directly beneath an access-controlled
evidence root outside this repository. The record binds an opaque local record
ID, completed redaction status, retention-until date, and an opaque
deletion-procedure identifier with no path syntax.
For each of the six layouts, it stores only an opaque evidence ID, SHA-256,
pixel dimensions, and captured timestamp; no screenshot path or image bytes is
read or copied by the verifier. `scripts/run_skin_acceptance.py validate`
checks the clone-independent schema and payload boundary without a device.
`verify` additionally requires every status to be `pass`, the expected clean
measurement commit, matching external metadata, and an evidence root that is
not contained by the repository. The external JSON has a fixed top-level
schema: completion IDs, six screenshot metadata records, all three complete
180-second repetitions for every frozen scenario/layout after its 30-second
warm-up, the bounded per-run telemetry/CPU/missed-presentation/memory/render-I/O
facts, a baseline plus exactly ten post-destruction resource samples, and the
single frozen negative-sandbox result. It carries no provenance URL, host path,
account, device name, UDID, image payload, or unrecognized field. Each run is
bound to the manifest's chart/autoplay, activation/configuration, and expected
guard-vector digests; the verifier rejects overflows, incomplete/mismatched
samples, nonzero passing render-I/O counters, threshold violations, resource
growth, or a negative result that differs from its exact diagnostic, action,
counter, or equal-overlay-digest contract.

### Resident-memory provenance

`SkinResourceLifecycleSample::residentBytes` is optional at runtime. When no
platform process-residency probe is available, the recorder omits the field
from its lifecycle JSON; it never substitutes zero, decoded bytes, or uploaded
resource bytes. Schema-v1 external evidence deliberately remains stricter and
requires a measured numeric `residentBytes` value for its baseline and every
post-destruction sample. Consequently, an export with omitted residency remains
honest but cannot satisfy final physical acceptance until a real process-memory
measurement is supplied. This preserves the frozen schema-v1 compatibility
contract rather than treating unmeasured memory as proof of no drift.

## Audited compatibility decision and remaining work

The selected 7-key entry's loaded closure critically imports Beatoraja's
restricted legacy `luajava` facade for package file behavior. The reviewed v1
surface contains one unguarded `java.io.File` class bind, two File-constructor
sites, one configured-load `listFiles` site, and one deferred `mkdir` site. It
does not import Gdx or use its audio facade. The v1 implementation exposes only
the corresponding closed non-Java virtual File behavior; URL/HTTP,
Java/reflection, controllers/input, native access, host paths, and every
unaudited class/member remain denied. Physical screenshots remain permitted by
the author terms, but no screenshot counts as passing evidence until the exact
facade, restricted `dofile`/`io.open`, renderer, sandbox, and all other
schema-v1 criteria are implemented and pass.
