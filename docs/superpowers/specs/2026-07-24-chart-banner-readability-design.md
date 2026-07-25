# Chart Banner Readability Design

## Goal

Keep chart-row titles, metadata, rank indicators, and favorite controls readable over arbitrary `#BANNER` artwork without hiding the artwork or changing rows that have no banner.

## Chosen approach

Extend the existing image fade shader with an optional theme-aware scrim. The shader blends the sampled image toward a supplied scrim RGB color by the scrim alpha, then applies the existing directional alpha fade. Because the final image alpha still follows the directional fade, the scrim has no separate rectangular edge and remains inside the image's rounded geometry.

Alternatives considered:

- Separate translucent chips behind every control provide strong local contrast but make the row visually fragmented.
- Text shadows or outlines help labels but do not protect icons, rank surfaces, or other translucent UI.

The shader scrim is the smallest consistent treatment and matches the existing shader-based banner rendering.

## Common component API

`ImageView` owns optional scrim state independently of its optional directional fade. It exposes:

- `setScrimColor(const Color &color)` for a fixed RGBA scrim.
- `setThemedScrimColor(ThemeColorProvider provider)` for a scrim that follows theme changes.
- `clearScrimColor()` to return to unmodified image color.
- `scrimColor()` to inspect the current optional color in tests and consumers.

The themed setter evaluates immediately. `ImageView::onThemeChanged()` calls `View::onThemeChanged()` and reevaluates the provider. Clearing the scrim also clears the provider.

The specialized image shader is selected whenever a fade or scrim is active. Missing fade state is represented by zero fade strength, and missing scrim state is represented by zero scrim alpha, so either treatment can be used alone.

## Shader contract

The existing `u_imageFadeParams` direction, offset, and strength contract remains unchanged. A new `u_imageScrimColor` vec4 contains normalized RGBA values.

For every sampled pixel, the fragment shader:

1. Clamps the supplied scrim alpha.
2. Blends sampled RGB toward the supplied scrim RGB using that alpha.
3. Computes directional progress and fade strength exactly as before.
4. Multiplies only the sampled alpha by the directional fade multiplier.
5. Outputs the treated color.

No Windows shader binary is added. Metal, SPIR-V, and GLES binaries are regenerated from the updated source.

## Chart-row treatment

Only eligible rows that already bind a `#BANNER` use the readability scrim. The banner remains the first child of the content card and therefore stays behind all UI.

- Dark theme: use `Color(5, 10, 18, 144)`.
- Light theme: use `Color(255, 255, 255, 168)`. The stronger light-theme
  alpha protects the lower-contrast muted foreground colors.

Selected and unselected rows use the same scrim; their existing foreground
and card-state styling remains responsible for selection feedback.

Rows without a banner, course rows, unavailable rows, and solid-archive rows continue to render without banner artwork. Regular `ImageView` instances have no scrim by default.

## Testing and verification

- Extend the shader source audit to require the scrim uniform, alpha clamp, RGB blend, unchanged directional alpha fade, and output.
- Extend `image_view_fade_tests` to verify fixed scrim state, themed reevaluation, clearing, and coexistence with fade state.
- Extend `chart_list_item_view_tests` to verify the banner has the expected theme-aware scrim and updates when the active theme changes.
- Recompile Metal, SPIR-V, and GLES shader binaries with the repository shader compiler.
- Run the focused fade/banner tests, the full CTest suite, and the desktop `main` build.
