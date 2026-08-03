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
  activated-revision digests, with configuration and activated-revision values
  explicitly pending until physical evidence exists;
- synthetic chart hashes and fixed autoplay-script hashes for normal notes,
  every supported LN/CN/HCN phase, BPM/stop/scroll changes, chords, all judgment
  grades, combo breaks, gauge thresholds/failure, lane cover, BGA transitions,
  and song end;
- screenshot timestamps and a per-layout external evidence reference, without
  image bytes in Git;
- a selected-ID timer/event trace that records the observed `IntMap`
  backing-hash order while still requiring the timer phase before the event
  phase;
- all six 16:9 and 4:3 Fit/Stretch/Custom layout cases;
- a 30-second warm-up followed by three complete 180-second repetitions for
  every scenario/layout; and
- `pending|pass|fail` plus an evidence reference for every completion criterion.

The thresholds are p99 skin CPU time at or below 90% of the actual refresh
interval, missed presentations at or below 0.5%, no active-render filesystem
reads or uploads, no live texture/resource growth after ten completed exits,
and no more than 32 MiB resident-memory drift after warm-up.

Task 1 permits `pending`. Final acceptance permits only `pass`.

## Current blocker recorded by the audit

The selected 7-key entry's loaded closure critically imports Beatoraja's
restricted legacy `luajava` facade for package file/audio behavior. The approved
AsoBMaShow v1 sandbox intentionally exposes no `luajava` and no network. This is
recorded as a critical compatibility gap rather than a silent stub. Physical
screenshots remain permitted by the author terms, but a screenshot cannot count
as passing unmodified-skin evidence until the design conflict is explicitly
resolved and all criteria pass.
