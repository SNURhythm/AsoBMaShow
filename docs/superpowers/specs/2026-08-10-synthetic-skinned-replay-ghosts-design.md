# Synthetic Skinned Replay Ghosts Design

## Goal

Show replay ghost outlines when a Beatoraja gameplay skin is selected, in both
interactive replay watch and replay-video export.  This is an explicit
application overlay, not a claimed Beatoraja skin feature: pinned Beatoraja
does not expose replay ghost objects to play skins.

The existing Replay Ghosts option remains the sole control.  When disabled,
selected skins render no synthetic ghosts.  Built-in presentation retains its
existing native ghost renderer.

## Existing Boundaries

`BMSRenderer` builds replay ghost events with `ReplayGhostUtils` and draws an
outline using built-in lane dimensions.  That outline cannot be submitted for
a selected skin because its geometry does not match the skin's note lanes.

`PlayfieldPresentationCoordinator` is shared by replay watch and by normal and
course replay-video export.  On a selected-skin frame it prepares the warmed
built-in candidate, then submits the selected `CoordinatedPlaySkinSession`.
`PlaySkinSession` lowers the selected `SkinNoteObject` using resolved,
per-lane `SkinLaneNotePresentation` data: lane rectangle, authored normal-note
height, clipping region, scroll mapping, and UI-to-authored transform.

## Design

Add a narrow, renderer-independent synthetic-ghost geometry publication to
`CoordinatedPlaySkinSession`.  The concrete Lua session publishes an immutable
snapshot only after its matching skin frame has evaluated successfully.  The
snapshot contains one entry per authored lane; it does not assume lane zero's
dimensions apply to another lane.  Each entry carries:

- the authored lane identifier;
- its resolved note rectangle (`x`, `width`, and normal-note height);
- its lane clip region and scroll direction/range; and
- the exact UI transform used for that frame.

The shared coordinator owns one synthetic replay-ghost overlay.  It receives
the same replay ghost events already constructed by `ReplayGhostUtils`, then,
only after a skin frame has been successfully submitted, projects each future
event with the geometry entry for that event's lane and submits a colored
outline above the skin.  The normal built-in path does not use the synthetic
overlay, so its existing renderer remains the only source of built-in ghosts.

Normal and course exports already route frames through
`ReplayPlayfieldPresentation` and the coordinator.  Replay watch does the
same through `GamePlayScene`.  Therefore the overlay is added at the
coordinator boundary rather than duplicated in either exporter or scene.

## Behavior and Failure Semantics

- Ghost event construction, ordering, timing window, and judgement coloring
  reuse `ReplayGhostUtils`; geometry is the only selected-skin-specific part.
- Each ghost uses its own lane's resolved width, note height, scroll mapping,
  clip, and transform.  Mixed lane widths, offset lanes, and nonuniform note
  heights are supported.
- The overlay is submitted only for a successful `PresentationMode::Skin`
  frame and only while `replayGhostRenderingEnabled` is true.
- A missing/invalid lane geometry entry simply omits that event.  It neither
  changes skin validation nor turns an otherwise valid skin frame into a
  fallback or export failure.
- A failed selected-skin frame preserves the established diagnostics and
  no-fallback export behavior.  The overlay is never drawn over a failed or
  built-in-substituted selected-skin frame.
- This feature adds synthetic ghosts only.  It does not add synthetic replay
  touches or miss markers, because those were not requested.

## Tests and Verification

Focused tests will prove:

1. selected-skin geometry is published per lane, including unequal widths and
   note heights;
2. option-off submits no synthetic ghost commands;
3. option-on submits one outline at the matching lane's transformed geometry,
   after the selected skin frame;
4. replay watch and normal/course export all exercise the same coordinator
   overlay path; and
5. built-in replay ghosts are not duplicated.

The primary verification loop is focused desktop tests followed by the normal
desktop build.  Because this changes the shared native gameplay presentation,
the final checkpoint is `scripts/ios_release_verify.sh`; it performs no
deployment or upload.
