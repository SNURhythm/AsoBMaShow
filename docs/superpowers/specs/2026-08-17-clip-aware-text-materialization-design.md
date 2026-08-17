# Clip-Aware Text Materialization

## Goal

Avoid creating text textures for views that never reach the visible portion of
the settings tree, while retaining the existing scissor-safe draw culling.

## Design

`View::render` already suppresses a fully clipped view's decoration and
`renderImpl`; child traversal remains unconditional because an absolutely
positioned child may legitimately overflow into the scissor. That behavior is
preserved.

`TextView` will separate layout measurement from texture materialization.
Text and wrap changes synchronously calculate the measured rectangle needed by
Yoga, but only mark the texture stale. The first visible `renderImpl` creates
the SDL surface and bgfx texture, so a fully clipped text view incurs neither
rasterization nor GPU upload. A later visible frame submits the normal UI
batch unchanged.

Diagnostic history already creates one `TextView` for every
`SkinDiagnosticHistoryRecord`; it must keep that row-per-record construction
instead of concatenating the whole history into one texture.

## Safety

- Text measurement remains synchronous so layout geometry does not jump when a
  row becomes visible.
- Texture invalidation destroys an obsolete texture immediately and preserves
  current color/theme/wrap semantics.
- The existing scissor culling continues to use `renderingBounds`, preserving
  visible text overflow and transformed rendering.

## Verification

- A TextView source contract proves `setText` does not materialize a texture
  and visible rendering does.
- Existing view-layout tests retain the absolute-child clipping guarantee.
- A settings source contract proves diagnostics stay one TextView per history
  record.
