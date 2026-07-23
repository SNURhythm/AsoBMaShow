# Chart List Banner Background Design

## Goal

Render a chart's parsed `#BANNER` image as decorative artwork on the right side of each main chart-selection row. The banner fades from invisible at its left edge to a subdued background image at its right edge so row content remains legible.

## Scope

- Apply the feature to `ChartListItemView`, which backs the main chart recycler in `MainMenuScene`.
- Keep the existing `#STAGEFILE` jacket and frame on the left unchanged.
- Reuse the existing `bms_parser::ChartMeta::Banner`, `Folder`, and asynchronous `ImageView` loading path. Parsing and repository persistence already support `#BANNER` and require no changes.
- Do not add banners to music-player, IR-upload, result, replay, folder, or modal list rows.

## Selected Approach

Add an optional horizontal alpha-fade style to the common `ImageView` component. When an image is decoded or obtained from the shared cache, `ImageView` creates the GPU texture from an RGBA copy whose alpha channel is multiplied by a linear left-to-right opacity ramp. The shared decoded cache remains unmodified, so the same source image can still be displayed normally by other `ImageView` instances.

This approach is preferred over a new fragment shader because it reuses the current cross-platform texture program and avoids adding compiled shader artifacts for Metal, Vulkan, GLES, and DirectX. It is preferred over a card-colored overlay because the fade is real transparency and is independent of the active UI theme.

## Component Design

### Alpha-mask utility

Create a small image-processing utility that accepts RGBA bytes, image dimensions, and left/right opacity values. It returns a transformed RGBA copy with:

- RGB channels preserved exactly.
- Source alpha multiplied by a linear opacity interpolation across each row.
- Opacity inputs clamped to `[0.0, 1.0]`.
- Invalid dimensions or byte counts rejected by returning an empty result.
- A one-pixel-wide image using the left-edge opacity, since it has no horizontal span.

Keeping this math outside `ImageView` makes it independently testable without a rendering context.

### `ImageView` property

Add a common-component setter for an optional horizontal alpha fade. The property stores the clamped left and right opacities and is applied whenever a cached, synchronous, thumbnail, or asynchronous image becomes a texture. Clearing the property restores normal rendering on the next texture application.

Changing the property while an image is already available reapplies the cached decoded pixels immediately. Pending asynchronous loads retain the configured property and apply it when decoding completes. The cache continues to store original RGBA bytes, never faded bytes.

### Chart-row banner layer

Each `ChartListItemView` owns a named `ImageView` banner layer inserted as the first child of the row's `contentCard`. It is absolutely positioned against the right edge, inset inside the one-pixel border, sized to the card's inner height and a fixed width close to the common BMS banner aspect ratio. Because it is inserted before all normal row children, the clear lamp, jacket, text, score rank, difficulty, and favorite control render above it.

The banner uses a left opacity of `0.0` and a right opacity of `0.48`. This keeps the right edge visibly identifiable while treating the artwork as background rather than foreground content. Its right corners follow the card's inset corner radius.

## Data Flow

When `ChartListItemView::setMeta` binds a normal available chart with a non-empty `meta.Banner`, it requests `meta.Folder / meta.Banner` through `setImageAsync`. When the row is rebound to a chart without a banner, an unavailable chart, or a solid archive placeholder, it calls `freeImage` so neither an old texture nor its image identity can leak from the recycled row.

Archive-member paths continue to work through the existing `ImageView` and `ArchiveFile` decoding path. A failed or missing banner leaves the background empty without affecting row interaction or metadata.

## Rendering and Interaction

- The banner is decorative and has no event handler.
- Existing selection and unselection backgrounds remain visible through the transparent fade.
- The content-card border remains unobscured because the banner is inset by one pixel.
- The banner does not participate in flex layout, so it cannot displace or resize row content.
- Selection, favorite toggling, score-rank display, marquee text, and recycler sizing remain unchanged.

## Testing

Add focused tests for:

1. Horizontal alpha-mask interpolation, source-alpha multiplication, RGB preservation, clamping, one-pixel width, and invalid buffer handling.
2. `ImageView` horizontal-fade property storage and clamping.
3. `ChartListItemView` binding the expected `Folder / Banner` path, anchoring the banner on the right as an absolute background layer, configuring the approved fade, and placing row content above it.
4. Rebinding a recycled row to no banner, unavailable metadata, or a solid archive clearing the prior banner identity.

Run the focused targets first, then the complete CTest suite and the repository's desktop `main` build. Compile shaders only if shader sources change; this design does not require shader changes.
