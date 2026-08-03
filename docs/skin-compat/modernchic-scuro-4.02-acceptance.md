# ModernChic/SCURO 4.02 acceptance record

Status: **pending**. Task 1 pins the source/package contract; milestone closure
requires every schema-v1 completion criterion to be `pass` with external
evidence.

## Official package provenance

| Field | Pinned value |
| --- | --- |
| Product/version | ModernChicPlay (SCURO) 4.02 |
| Official release page | <https://www.kasacontent.com/musicgame/beatoraja/4226/> |
| Official download collection | KasaBlog-linked Google Drive folder `1OZYZ09n1XKIcStIW9LboYlVR659vtxg6`, historical `before` folder |
| Exact archive filename | `ModernChic402.zip` |
| Archive bytes | `445591453` |
| Archive SHA-256 | `06ad5a4c5a1b6d0ece08b79475cbe2b4a5187ce07e490752e141518ee4fcc41c` |
| Acquisition/access date | 2026-08-03 |
| Canonical archive package prefix | `ModernChic` |
| Corresponding extracted root identity | `skin-tree:448a5b031b153c3424e5d5ca6b07ab1fe832dabbe18c0faf6f72bc17bc4af18d` |
| Archive payload `SkinTreeDigestV1` | `448a5b031b153c3424e5d5ca6b07ab1fe832dabbe18c0faf6f72bc17bc4af18d` |
| Extracted source `SkinTreeDigestV1` | `448a5b031b153c3424e5d5ca6b07ab1fe832dabbe18c0faf6f72bc17bc4af18d` |
| Selected 7-key entry | `play7_hw.luaskin` |
| Selected entry identity | `entry-d5399e62255ddbda273e7a63` |
| Selected entry SHA-256 | `aac73c59526e74f159731608b3b54e58a7e534537503acb88565a4c175cf8f13` |
| Selected Lua closure `SelectedLuaClosureContractV1` SHA-256 | `717b46b6641c84e431490fff24f45a0ee23a1208017cc4dae4ea2cad438f5bb0` |

The archive and extraction live outside the repository. The equal, independently
computed tree digests bind the audited source tree to the exact official ZIP.
The repository contains only hashes, opaque inventory IDs, public provenance,
and the selected entry identity.

## Author terms and screenshot decision

The authoritative package readme inside the hashed official archive permits
use of the included data, prohibits redistribution of the skin itself without
permission, requests KASAKO attribution for published modifications, and
disclaims responsibility for modified skins. The author-published usage page
at <https://www.kasacontent.com/musicgame/beatoraja/4635/> (accessed
2026-08-03) explicitly describes ModernChic features intended for YouTube/OBS
streaming. The release page also publicly illustrates gameplay screenshots.

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
- synthetic chart hashes and fixed autoplay-script hashes for normal notes,
  every supported LN/CN/HCN phase, BPM/stop/scroll changes, chords, all judgment
  grades, combo breaks, gauge thresholds/failure, lane cover, BGA transitions,
  and song end;
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
`eb52ec905a507efe50273b2ff09d91d10f025d5019c7bf3d4330ee31e3445e13`;
both opaque render-I/O guards are `not-reachable`. The negative audit vector is
`e4f5da4b24ae219320023f32820569fb442ceb0b6ad0a4823de74325ddc07552`;
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
source-constant, manifest, and acceptance review. The scanner tests characterize
this pinned SCURO 4.02 closure and do not claim general Lua verification.

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

## Audited compatibility decision and remaining work

The selected 7-key entry's loaded closure critically imports Beatoraja's
restricted legacy `luajava` facade for package file behavior and a guarded
audio probe. The reviewed v1 design now resolves only that exact surface with a
closed non-Java Lua table: virtual File listing, overlay-only latent `mkdir`,
and a GDX table whose absent `app` preserves pinned Beatoraja's optional audio
failure. URL/HTTP, Java/reflection, controllers/input, native access, host paths,
and every unaudited class/member remain denied. Physical screenshots remain
permitted by the author terms, but no screenshot counts as passing evidence
until the exact facade, restricted `dofile`/`io.open`, renderer, sandbox, and all
other schema-v1 criteria are implemented and pass.
