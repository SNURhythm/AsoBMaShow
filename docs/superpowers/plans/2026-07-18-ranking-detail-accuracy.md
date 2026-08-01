# Native Bokutachi Ranking Detail Implementation Plan

**Goal:** Replace fabricated beatoraja-compatible ranking details with native
Tachi PB data and use KPOOR-inclusive BP only where the UI says BP.

## Constraints

- Bokutachi remains the only active network ranking provider.
- Result-screen BREAK remains `BAD + POOR` and is not modified.
- API keys stay in profile runtime configuration and never enter cached rows or
  outbox payloads.
- Historical native PBs may omit timing details; missing evidence is displayed
  as unavailable, never zero or synthesized data.

## Tasks

- [x] Reproduce the fabricated `epg/lpg/egr/lgr` conversion in upstream Tachi.
- [x] Add red tests for native identity, chart resolution, PB parsing, authentic
      judgement counts, paged ranking windows, and request construction.
- [x] Resolve BMS charts through the native SHA-256 resolver.
- [x] Fetch the authenticated user ID and the first native PB page at rank 1.
- [x] Parse native score/lamp/BP/combo/time and optional timing metrics with
      complete response validation.
- [x] Keep the modal's unavailable state for historical scores without all four
      timing fields.
- [x] Treat partial or EX-inconsistent legacy timing as unavailable without
      rejecting the otherwise valid PB row.
- [x] Add complete replay timing fields to Direct Manual submissions while
      retaining PGREAT-excluded aggregate fast/slow.
- [x] Load local PB as `BAD + POOR + KPOOR` using the selected LN mode.
- [x] Calculate result-modal BP separately from unchanged combo BREAK.
- [x] Run the focused suite, complete CTest suite, desktop build, and diff checks.

## Virtualized Pagination Follow-up

- [x] Add a provider-neutral opaque next-page token to ranking results and a
      driver method that fetches one subsequent page.
- [x] Make Tachi's initial request return rank 1 immediately and encode the
      resolved chart ID, authenticated user ID, stable `outOf`, loaded
      position, and previous rank in a credential-free in-memory token.
- [x] Fetch subsequent native PB pages with one HTTP request per token; reject
      malformed tokens, changed `outOf`, duplicate users, or regressing ranks.
- [x] Let `IrRankingService` append successful pages while retaining the
      visible ranking during page loading or page failure.
- [x] Trigger `loadNextPage` when the virtualized list approaches its final
      ten rows, with one in-flight page at a time and no automatic failure loop.
- [x] Keep inconsistent legacy timing optional and unavailable rather than
      rejecting an otherwise valid row.
- [x] Run focused IR tests, the full CTest suite, desktop build, and diff checks.
