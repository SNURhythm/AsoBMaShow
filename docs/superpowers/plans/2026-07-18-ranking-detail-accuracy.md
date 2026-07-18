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
      judgement counts, pagination, and request construction.
- [x] Resolve BMS charts through the native SHA-256 resolver.
- [x] Fetch the authenticated user ID and paginated native PB documents.
- [x] Parse native score/lamp/BP/combo/time and optional timing metrics with
      complete response validation.
- [x] Keep the modal's unavailable state for historical scores without all four
      timing fields.
- [x] Add complete replay timing fields to Direct Manual submissions while
      retaining PGREAT-excluded aggregate fast/slow.
- [x] Load local PB as `BAD + POOR + KPOOR` using the selected LN mode.
- [x] Calculate result-modal BP separately from unchanged combo BREAK.
- [x] Run the focused suite, complete CTest suite, desktop build, and diff checks.
