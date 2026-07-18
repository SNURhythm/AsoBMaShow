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
3. Page through
   `GET /api/v1/games/bms-{7k|14k}/charts/:chartID/pbs?startRanking=N`.

The native response supplies server ranks, users, EX score, lamp, PB, max
combo, timestamp, and optional `epg/lpg/egr/lgr`. The parser validates the
resolved game, hash, note count, chart ID, user mapping, page order, `outOf`,
score range, and the EX-score equation when all four timing values exist.
Historical scores without a complete timing breakdown remain valid and show
the existing unavailable message.

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
- Each native PB page is capped at 8 MiB and 100 entries; the complete ranking
  is capped at 20,000 entries.
- Pagination must remain ordered and reach the server's stable `outOf` count.
  A ranking that changes or becomes incomplete during fetch is rejected rather
  than shown partially.
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
