# Gameplay text atlas

Changing score, combo and timing labels currently rasterizes and uploads an
entire string. Add an opt-in `TextView` path that reuses glyph textures while
SDL_ttf supplies shaping, kerning and glyph placement. Existing layout,
alignment, clipping, font choices and fallback rendering remain authoritative.

Use a shared, bounded atlas per live font/generation, retained only by opted-in
views. Keep only the current shaped string per view. Upload a glyph once, append
quads through the existing UI batch, and tint with the existing image-fade
shader. Never overwrite occupied atlas regions. Unsupported text, wrapping,
overlapping glyphs or exhausted atlas capacity use the whole-string renderer.
Release text and atlas state before releasing fonts. No shaders or dependencies
need changing.

Implementation and verification:

1. Extend the Metal text test with legacy/atlas pixel comparisons and a warmed
   label texture-reuse assertion. Observe failure before implementation.
2. Implement the opt-in path, including bounded storage, padding, generation
   invalidation and complete preparation before any glyph submission.
3. Enable it for changing built-in gameplay labels and style replacements.
   Update the existing deferred-HUD regression to cover this behavior.
4. Run focused text, UI batch and built-in renderer tests, compile desktop and
   unsigned iOS, then obtain an independent review and commit/push to PR #161.

Review focus: font/session lifetime, generation changes, texture mutation while
commands are pending, fractional filtering, empty/unsupported strings, clipped
and transformed labels, atlas capacity and hidden-view work.

Completed: regression failures were observed before implementation; all five
focused suites, desktop main and unsigned iOS build pass. Independent review is
clean. The detailed implementation limits and pixel evidence are recorded in
[the input-feedback report](../reviews/2026-10-28-input-feedback-rendering-inspection.md#implemented-numeric-text-follow-up).
