# Modern Chart BRD Recording and Playback Design

## Scope

This is the focused design for delivery Slice 4 of the approved
contract-first file replay restart. It activates modern chart recording,
persistence, recall, and replay-dependent chart consumers. Course attempts
remain on their existing path until Slice 5. Legacy playback rows remain
readable until the summary-only transition in Slice 7.

## Selected Architecture

Schema version 11 is an additive transition. It creates strict modern chart
result, IR snapshot, replay-file reference, and reservation tables beside the
schema-v10 legacy tables. A chart completion no longer calls the legacy replay
writer. It creates three independently validated values:

1. a `ModernChartResult`, captured directly from completed gameplay;
2. an optional `IrSubmissionSnapshot`, captured from that result; and
3. a `ReplayChartDocument`, captured from raw logical input and visual replay
   extensions.

The replay file coordinator encodes the document, reserves a deterministic
Beatoraja history slot, installs and verifies the BRD, and then asks the
repository to associate the result, snapshot, outbox work, and file metadata
in one SQLite transaction. Exact retries agree on attempt ID, result
fingerprint, path, compressed hash, size, and codec version. A conflicting
retry fails closed. If SQLite rejects an installed file, cleanup may remove it
only when the installed bytes still match the unassociated ownership receipt.

An additive transition was selected over writing empty legacy replay headers:
empty legacy rows would keep two result authorities and make a modern chart
look replay-backed in SQLite. Deferring all modern persistence to Slice 7 was
also rejected because chart result recall and postponed IR must be durable as
soon as chart BRDs become active.

## Raw Input Authority

`LogicalGameplayInputAdapter` remains the only authority that converts
resolved logical actions into effective gameplay lane operations. It gains an
observer for accepted effective transitions. The observer sees lane presses
and releases, scratch direction ownership and synthetic reversal handoffs,
Start, and Select after the adapter has rejected invalid or redundant input.
Pause, Retry, and lane-cover commands are not encoded as stock key input.

`ReplayInputRecorder` converts those accepted transitions into the canonical
`replay::InputTransition` model at the current signed song time. It appends in
arrival order and never sorts. A time reversal, unsupported action, count
overflow, or capture truncation permanently invalidates that attempt's replay
attachment; the compact result and postponed IR snapshot remain saveable.
Realtime and ordinary input paths both pass through the same adapter observer.

Touch samples and timed lane-cover values are recorded only in the versioned
AsoBMaShow BRD extension. Their producers use the same time and count limits as
the codec. No raw input, touch, or lane-cover item is written to SQLite.

## Setup and Agreement

The chart producer creates one canonical `ReplaySetup` from the exact live
configuration. Replay/result shared facts are compared by one agreement
function before file association and again before replay actions are exposed.
The agreement covers parsed chart hashes, key mode, effective long-note mode,
play options and seeds, double-play FLIP, assist, gauge configuration, ruleset,
playback rate, and score provenance.

A consumer first resolves and parses the selected chart, derives its identity
and time bounds, reads verified bytes through `ReplayFileStore`, decodes with
the chart key mode, validates the decoded setup against the parsed chart, and
checks result/setup agreement. Stored display metadata never replaces a
contradictory parsed identity. Missing, corrupt, unsafe, mismatched, or
unsupported files yield a replay-unavailable context while leaving the modern
result usable.

## Playback Context and Consumers

One immutable `ChartReplayContext` contains the modern result, decoded
playback, parsed chart identity, availability state, and diagnostic. Watch,
Retry Same, G-Battle, practice ghost, and video export receive this context;
they do not load, decode, or translate replay data independently.

`ReplayPlaybackDriver` advances the raw input stream monotonically at gameplay
song time and feeds logical transitions back through the same gameplay input
adapter. `ReplayPlaybackMaterializer` uses that driver and normal gameplay
rules to generate judged analysis when a consumer needs it. The materialized
outcome may be compared with the saved modern result but can never overwrite
or reconstruct it. Retry Same applies only the validated setup. Watch and
video run the raw driver. G-Battle and practice ghost consume judged analysis
derived from the same context.

## Failure and Recovery

- Encoding, reservation, install, or verification failure saves the modern
  result and eligible IR snapshot without a replay reference.
- Database rollback after installation leaves no reference. The coordinator
  removes only an exact hash-and-size match bearing the current reservation;
  ambiguous ownership is preserved for later reconciliation.
- An interrupted exact retry reuses the reservation and identical installed
  bytes. A changed result or file is an integrity conflict.
- A referenced file that is later absent or corrupt disables only
  replay-dependent actions. Deletion and sharing are activated in Slice 6.
- Result and IR readers never open replay files.

## Slice Gate

Slice 4 is complete when new chart attempts create a validated BRD and strict
modern database rows without new legacy event, touch, or lane-cover rows; chart
result recall and postponed IR work with the BRD removed; all chart replay
consumers use the shared context; focused contract/integration tests pass; and
the complete diff review against `origin/develop` finds no duplicate setup,
identity, result-agreement, limit, or file-ownership authority.
