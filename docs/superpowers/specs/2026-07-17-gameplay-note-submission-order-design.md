# Gameplay Note Submission Order Design

## Goal

Mirror Beatoraja's note overlap behavior without assigning permanent depths by
note type. Rendering order follows the order in which chart rows are traversed.
The only deferred-order exception is a long note: its body and tail retain the
order captured at its head row.

## Render-order model

`BMSRenderer` will use one lightweight submission-order allocator shared by the
textured-note and solid-invisible-note batchers. The allocator resets at the
start of every frame and returns monotonically increasing bgfx depth tokens.
Call sites do not calculate depths from a note type or a timeline index.

Each traversed row submits its logical phases in Beatoraja order:

1. playable notes and landmines;
2. invisible notes, when their display setting is enabled.

The batchers allocate a new token when the renderer advances to the next
logical phase. Consequently, a later row sorts above an earlier row, while an
invisible note sorts above a playable note or landmine on the same row. Rows
without drawable notes do not consume tokens.

Textured and solid phases must not share a depth token. The main bgfx view uses
depth sorting, and equal-depth submissions from different shaders are then
ordered by program rather than by call order. Unique monotonic tokens make the
cross-batcher result deterministic.

## Long-note exception

When traversal reaches a long-note head, the renderer captures the head row's
order and stores it with the existing long-note lookahead entry. Long-note
geometry may still be produced when traversal reaches the tail, because only
then are both rendered endpoints available, but it is submitted with the
captured head order.

The head order reserves an internal body token immediately before its primary
token. The body uses the body token; the head and tail use the primary token.
The head and tail must therefore always render above their own body, including
when the body and endpoints use different textures or the tail geometry is
produced later. This preserves the body-behind-endpoints relationship without
moving the long note to the tail row. Invisible notes in the head row and all
notes in later rows receive subsequent tokens.

An already-active long note whose head precedes the current render window gets
an earliest-frame order before the first traversed row. A future long note that
extends beyond the visible traversal retains its captured head order when the
leftover body is rendered.

## Batching

The implementation will stream through one reusable textured-note batch and
the existing solid-note batch. Changing a depth token flushes pending geometry
before changing the batch's submit depth; texture changes retain the existing
texture-flush behavior. This avoids a renderer or command queue per row and
keeps memory use independent of chart length.

Landmines stored in the parser's separate landmine array are submitted before
the invisible-note phase. This makes their order follow traversal rather than
the temporary fixed landmine depth introduced previously.

The note-order token range sits above lane beams and below ghosts and world
overlays. Existing fixed type depths for landmines, invisible notes, long
bodies, and normal notes are removed. Overlay constants are adjusted only as
needed to preserve their existing placement outside the note-order range.

## Tests

Unit tests will verify that:

- successive logical submissions receive increasing tokens;
- a new frame resets the allocator;
- an invisible phase follows the primary phase of the same row;
- later rows follow all phases of earlier rows;
- long body, head, and tail reuse the order captured at the head rather than
  the tail row;
- a long note's head and tail always sort above its own body;
- lane beams remain below the note token range and ghosts/overlays remain
  above it.

The desktop target and complete CTest suite will be run after implementation.

## Scope

This change affects only note overlap ordering and the batching needed to
express it. It does not change note geometry, judge-line clipping, expiration,
invisible-note appearance, gameplay timing, or chart parsing.
