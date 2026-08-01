# File Replay Codec and Contained Store Design

## Scope

This is delivery Slice 2 of the contract-first file replay restart. It adds
Beatoraja-compatible replay bytes, Beatoraja path grammar, and a contained file
lifecycle without activating gameplay persistence or changing SQLite.

The Slice 1 setup, playback, time, and resource contracts remain authoritative.
This slice consumes those types and does not introduce parallel codec limits,
setup validation, or input validation.

## Selected Boundaries

### Replay document

A replay document is playback evidence plus the timing context needed to
validate it. A chart document contains one `ReplayPlaybackData` and one
`ReplayTimeBounds`. A course document contains `CourseReplayPlaybackData` and
one bound per stage. Completion time is playback context, not a result fact.

The stock Beatoraja envelope owns interoperable fields. The versioned
`asobmashow` extension owns only playback facts that stock Beatoraja cannot
represent: exact canonical setup, signed pre-roll preservation, touch samples,
timed lane-cover changes, course rest, and validation bounds. It contains no
score, judgement result, provenance, fingerprint, database ID, path, IR, or
delivery state.

Decoding never lets extension fields silently replace contradictory stock
identity. When a fact exists in both representations, the two projections must
agree. An unsupported extension is distinguished from corrupt stock data so a
future AsoBMaShow file can remain a valid stock replay surface.

### Stock projection

Encoding always emits a valid stock representation even when an exact Aso-only
fact also lives in the extension. Manual lane assignment projects to stock
`NORMAL` and is preserved exactly in the extension. Stock
`laneShufflePattern`, random seeds, DP `doubleoption`, starting gauge, initial
lane cover, and compact signed key input have direct contract tests.

Completed replays may contain no input transitions. Negative pre-roll times
are legal down to the shared inclusive limit. Stock-only decoding receives the
selected chart's key mode and completion bound from its caller; it does not
invent either from display metadata or the last input event.

The implementation is pinned to Beatoraja commit
`5f46fe198e88abbefe9215ca2de397aef8f54bd8`. Independent Java-generated chart,
course, and compact-key fixtures are retained with their generator and hashes.

### Path identity

Paths are relative to the active profile and have the form
`replay/<stock-stem>[_<history-index>].brd`. Chart stems use the full lowercase
SHA-256 and the Beatoraja undefined-LN prefix. Course stems concatenate the
first ten lowercase SHA-256 characters for each stage and append supported
constraint IDs.

The shared `ReplayLimits` owns both course stage and filename bounds. A course
with up to 256 stages is structurally valid playback, but path creation rejects
any stem that cannot leave room for the largest supported numeric history
suffix within 255 filename bytes. No truncation or alternate private naming
grammar is introduced.

Matching uses parsed chart identity and an explicit authored-undefined-LN
context. Unknown authored context may accept either compatible stock prefix;
known context requires exact agreement.

### Contained file lifecycle

The file store is rooted at one normalized profile directory and accepts only
canonical `replay/...brd` identities produced by the path module. Every
component is checked without following symlinks. Absolute paths, traversal,
alternate separators, non-regular files, links, and paths outside the profile
fail closed.

The lifecycle states are:

1. `Reserved`: a caller has selected a path and attempt token, but no durable
   file is trusted.
2. `TemporaryWritten`: private bytes were written and flushed under a filename
   derived from that reservation.
3. `InstalledUnassociated`: rename and parent-directory sync completed and the
   installed bytes match the expected hash and size.
4. `Associated`: a later coordinator has atomically associated the exact path,
   hash, size, codec version, and attempt with one modern result.
5. `Finalized`: association acknowledgement has been reconciled.
6. `Abandoned`: ownership was not established; cleanup may remove only the
   exact installed or temporary artifact proven by the reservation metadata.

Slice 2 implements reservation through verified installation and the pure
state transitions. It deliberately cannot claim `Associated` by itself. Slice
3/4 persistence will supply the database acknowledgement and will use the same
transition function.

An occupied destination is never overwritten by the isolated store. An exact
same-attempt, same-hash installed file is an idempotent retry. Replacement of a
different owner is deferred until a coordinator can reconcile both database
references. Ambiguous rename or directory-sync outcomes are re-inspected and
the parent is re-synced before success can be acknowledged.

Inspection distinguishes verified, missing, corrupt, unsafe, and I/O-failure
states without deleting anything. Removal requires the expected metadata and
is safe for a user-deleted/missing file. Startup cleanup is not activated in
this slice; the later profile slice must call the store cleanup entrypoint.

## Failure and Recovery Contract

Fault injection covers reservation, temporary write/flush, rename, directory
sync, installed validation, acknowledgement, and cleanup. Before association,
failure leaves no owned replay. After verified installation but before a
database commit, the coordinator may retry the exact attempt or mark it
abandoned and remove only its proven artifact. Missing files never affect
result or IR state.

## Slice Gate

Slice 2 is complete when independent Beatoraja fixtures decode, locally
produced chart/course documents encode and decode through the Slice 1
validators, path boundaries are exact, and every injected store failure has a
deterministic recoverable state. Existing gameplay and repository behavior
must remain unchanged.
