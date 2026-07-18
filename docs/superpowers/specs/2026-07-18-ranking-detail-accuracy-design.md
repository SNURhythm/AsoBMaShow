# Native Bokutachi Ranking Detail Design

## Goal

Display authentic Bokutachi BMS PB details and use LR2 BP semantics without
changing the result screen's BREAK metric.

## Root Cause

Tachi's `/ir/beatoraja/charts/:sha256/scores` compatibility converter always
rewrites the four judgement timing fields as an EX-score encoding:

```text
epg = floor(score / 2)
egr = score % 2
lpg = 0
lgr = 0
```

Those values satisfy beatoraja's legacy score interface but are not a real
PGREAT/GREAT breakdown. AsoBMaShow treated them as literal judgements, so a
1284 score appeared as 642 early PGREATs.

## Chosen Design

Ranking reads use Tachi's native BMS API rather than its beatoraja compatibility
export:

1. Resolve the lowercase SHA-256 with
   `POST /api/v1/games/bms-{7k|14k}/charts/resolve` and
   `matchType: bmsChartHash`.
2. Read the authenticated numeric user ID from `GET /api/v1/status`.
3. Fetch the first native ranking page at rank 1.
4. Return a credential-free opaque continuation token when more PBs exist.
   The virtualized modal requests another page when its viewport approaches
   the final ten loaded rows. A continuation uses only the native PB route; it
   does not repeat chart resolution or identity lookup.

The native response supplies server ranks, users, EX score, lamp, PB, max
combo, timestamp, and optional `epg/lpg/egr/lgr`. The parser validates the
resolved game, hash, note count, chart ID, user mapping, page order, `outOf`,
and score range. Timing metrics populate details only when all four exist and
reproduce the EX score. Missing, partial, or inconsistent historical timing is
treated as unavailable without invalidating the rest of the PB.

Direct Manual submissions include `epg/lpg/egr/lgr` only when the replay
contains a complete PGREAT/GREAT timing breakdown. LR2oraja assigns an exact
zero timing to the early side; aggregate Bokutachi `fast` and `slow` continue
to exclude all PGREAT counts as required.

Local ranking comparisons use BP = `BAD + POOR + KPOOR`. Song select loads the
selected best score row on demand so it can read the stored judgement counts.
The result ranking comparison calculates the same sum from the current state.

The result screen's BREAK value is deliberately unchanged: it remains
`BAD + POOR`, with KPOOR excluded. Only fields explicitly labeled BP use the
KPOOR-inclusive sum.

## Safety and Failure Handling

- Authenticated requests never follow redirects and never persist API keys.
- Chart resolution is rejected unless game, SHA-256, and note count match the
  local query.
- Each native PB response is capped at 8 MiB and 100 entries. Only one page is
  in flight and pages are requested on demand instead of serially blocking the
  initial modal.
- Continuation tokens bind the normalized chart hash, resolved chart ID,
  authenticated user ID, stable `outOf`, loaded count, and previous rank. They
  contain no API key and are bounded before parsing.
- The returned page must be ordered, retain stable user identities, preserve
  `outOf`, and cannot regress from the previous rank. Duplicate users or a
  page that does not advance fail pagination while preserving loaded rows.
- Tachi exposes a rank cursor rather than a row offset. If a single tied-rank
  group exceeds the 100-row server limit, the API cannot enumerate the rest of
  that tie deterministically; the loaded-count check stops with the verified
  prefix visible instead of claiming the list is complete.
- HTTP 404 maps to chart-not-found, 401/403 to authentication-required, and
  retryable transport/HTTP statuses to transient failure.

## References

- Tachi native chart resolve and PB routes:
  <https://github.com/zkldi/Tachi/blob/233bc992f74cd314c8ef9bc2730d714904838dfc/typescript/server/src/server/router/api/v1/games/router.ts>
- Tachi PB document shape:
  <https://github.com/zkldi/Tachi/blob/233bc992f74cd314c8ef9bc2730d714904838dfc/typescript/common/src/types/documents.ts>
- Tachi compatibility conversion that fabricates timing fields:
  <https://github.com/zkldi/Tachi/blob/233bc992f74cd314c8ef9bc2730d714904838dfc/typescript/server/src/server/router/ir/beatoraja/charts/_chartSHA256/convert-scores.ts>
- LR2oraja early/late assignment (`mfast >= 0` is FAST):
  <https://github.com/wcko87/lr2oraja/blob/lr2oraja/src/bms/player/beatoraja/play/JudgeManager.java>
