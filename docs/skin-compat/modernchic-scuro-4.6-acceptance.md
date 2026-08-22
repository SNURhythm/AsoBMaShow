# ModernChic/SCURO 4.6 acceptance record

Automated gameplay compatibility status: **pass** (2026-08-22). External
physical iPad evidence remains **pending**; milestone closure still requires
every schema-v2 completion criterion to be `pass` with external evidence.

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
| Gameplay entries | `play5_hw.luaskin`, `play7_hw.luaskin`, `play10_hw.luaskin`, `play14_hw.luaskin` |
| Entry identities | `entry-f8c6b6f1b2914d602812d2ca`, `entry-d5399e62255ddbda273e7a63`, `entry-338bec476d1c2ae36e50cbb6`, `entry-517ffa1a30626f80272e3c43` |
| Entry SHA-256 values | `8e3155900120949562f02d9cceff9b8ef4bc4c1b80dd06774b810e8b9b91199f`, `aac73c59526e74f159731608b3b54e58a7e534537503acb88565a4c175cf8f13`, `86dfe492d57c936c53a41c18d4e3d92a50c5f7855a01931e96979799be25686e`, `690d1c6bcb259af502c6a7dac5b619d9037b5fa3e62361c7aefc43cc3b291f82` |
| Selected Lua closure `SelectedLuaClosureContractV1` SHA-256 | `9d2a5acafc57edd6fa958c86fd92b861b851d952581a6828c1beb5b7fde67c0d` |

The archive and extraction live outside the repository. The equal, independently
computed tree digests bind the audited source tree to the exact official ZIP.
The repository contains only hashes, opaque inventory IDs, public provenance,
and the selected entry identity.

## Automated gameplay compatibility evidence

The local, noncommitting verifier ran against the exact archive above and the
pinned Beatoraja checkout at
`c2ed5db1a46145ed10790c3872f717e95b59db9d`:

```bash
python3 scripts/verify_modernchic_gameplay_skin.py \
  --skin /Users/xf/Downloads/Skins/ModernChic460.zip \
  --beatoraja-root ../beatoraja
```

The verifier safely extracted into a temporary directory, repeated the
archive/tree/selected-closure audit, and loaded all four gameplay entries
through real `PlaySkinSession` instances in LN, CN, and HCN modes. It observed
the selected long-note property, lowered projected notes to draw commands,
compared their source slots with the generated pinned Beatoraja trace, and
compared decoded-model resource references with independently prepared catalog
identities. The existing 7-key session also evaluated three fixed gameplay
frames for graph, callback, and loading telemetry. The verifier emitted only a
sanitized JSON report and deleted the temporary package. The headless movie
device seam verifies materialization, decoded-memory accounting, frame
lifecycle, and owner-thread destruction without retaining or redistributing
movie bytes.

| Automated fact | 2026-08-22 result |
| --- | --- |
| Session publication | pass |
| Gameplay entry/mode sessions | 12 / 12 published (5/7/10/14 keys × LN/CN/HCN) |
| LN/CN/HCN selector indices | `0` / `1` / `2` for every entry |
| Pinned draw-slot comparisons | 12 / 12 exact, including HCN reactive/damaged bodies |
| Referenced/prepared resource identities | exact for image/movie and text sets in every cell |
| Unsupported diagnostics / selectors | 0 / 0 |
| Note-distribution graphs | 3 declared / 9 commands |
| Timing visualizers | 1 declared / 12 commands |
| BPM graphs | 3 declared / 3 commands |
| Hit-error visualizers | 1 declared / 3 commands |
| Resource preparation | complete; all referenced resources prepared |
| Image / font / movie / audio decodes | 22 / 5 / 1 / 0 |
| Texture uploads | 27 |
| Lua callback frames / maximum wall time / budget | 3 / 47 us / 6,000 us |
| Loading telemetry / independently measured load | 1,854,801 us / 1,858,455 us |
| Other diagnostic codes | `skin.renderer.text.glyph` |

The remaining glyph diagnostic is not an unsupported gameplay object, field,
API, or selector diagnostic and does not prevent frame publication or the four
required graph families. It is retained in the report rather than reclassified
or hidden.

The generated source/fixture trace pins the exact Beatoraja
`LaneRenderer.drawLongNote` rule and JVM-read JBMS constants, with the pinned
source file's SHA-256. Undefined model type `0`, `1`, or `2` selects Aso
mode `1` (LN), `2` (CN), or `3` (HCN), respectively; an explicit note type
`1`, `2`, or `3` overrides the model. The fixture also pins the exact body,
cap, reactive, and damaged slots for every mode, and its negative contract
rejects a cyclic mode shift.

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

## Frozen acceptance schema version 2

The machine-readable source of truth is
`tests/fixtures/beatoraja_skin/reference_manifest.json`. It freezes the
archive, payload-tree, selected-entry, selected-closure, configuration and
activated-revision identities; non-identifying device/display/build records;
the five redistributable fixture digests; four chart/autoplay scenarios; six
Fit/Stretch/Custom layouts; the 30-second warm-up and three 180-second runs;
and resource-lifecycle limits. Third-party payloads, host paths, screenshot
bytes, and personal identifiers remain outside Git.

`ordinaryRuntimeIo` is the source-aligned runtime record. The pinned
`SkinLuaAccessor.RestrictedIoLib` bounds paths lexically to the selected skin
root, then performs normal reads/writes/directory operations there; the pinned
`LuaSkinLoader` reloads the live selected closure. A completed manual run may
therefore record observed selected-root reads, writes, and directory scans from
configured load or render callbacks. It does not require a file-I/O denial,
guard digest, overlay digest, synthetic counter, or built-in fallback. The
observation stores only canonical operation kinds and an opaque evidence ID.

The limits are p99 skin CPU time at or below 90% of the actual refresh interval,
missed presentations at or below 0.5%, no live texture/resource growth after
ten completed exits, and no more than 32 MiB resident-memory drift after
warm-up. The selected-closure digest remains an input gate: a closure byte or
identity change requires explicit manifest and acceptance review. The scanner
characterizes this pinned ModernChic 4.6 closure only; it is not a general Lua
verifier.

Task 1 permits `pending`. Final acceptance permits only `pass`.

## Desktop review

The source-aligned desktop review is recorded in
[`beatoraja-lua-gameplay-final-review.md`](beatoraja-lua-gameplay-final-review.md).
It verifies the desktop and contract gates at `e4f9ee10`; physical evidence
remains pending.

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
warm-up, the bounded per-run telemetry/CPU/missed-presentation/memory facts, a
baseline plus exactly ten post-destruction resource samples, and the single
ordinary selected-root I/O observation. It carries no provenance URL, host
path, account, device name, UDID, image payload, or unrecognized field. Each
run is bound to the manifest's chart/autoplay and activation/configuration
digests. The verifier rejects overflows, incomplete/mismatched samples,
threshold violations, resource growth, or an I/O observation that differs from
the opaque, canonical operation record in the contract.

### Resident-memory provenance

`SkinResourceLifecycleSample::residentBytes` is optional at runtime. When no
platform process-residency probe is available, the recorder omits the field
from its lifecycle JSON; it never substitutes zero, decoded bytes, or uploaded
resource bytes. Schema-v1 external evidence deliberately remains stricter and
requires a measured numeric `residentBytes` value for its baseline and every
post-destruction sample. Consequently, an export with omitted residency remains
honest but cannot satisfy final physical acceptance until a real process-memory
measurement is supplied. This preserves the frozen schema-v2 compatibility
contract rather than treating unmeasured memory as proof of no drift.

## Audited compatibility decision and remaining work

The selected 7-key audit closure critically imports Beatoraja's
restricted legacy `luajava` facade for package file behavior. The reviewed v1
surface contains one unguarded `java.io.File` class bind, two File-constructor
sites, one configured-load `listFiles` site, and one deferred `mkdir` site. It
does not import Gdx or use its audio facade. The v1 implementation exposes only
the corresponding closed non-Java virtual File behavior; URL/HTTP,
Java/reflection, controllers/input, native access, host paths, and every
unaudited class/member remain denied. Physical screenshots remain permitted by
the author terms, but no screenshot counts as passing evidence until the exact
facade, selected-root `dofile`/`io.open`, renderer, and all other
schema-v2 criteria are implemented and pass.
