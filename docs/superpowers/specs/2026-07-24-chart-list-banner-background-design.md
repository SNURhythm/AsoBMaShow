# Chart List Banner Background Design

## Goal

Render a chart's parsed `#BANNER` image as decorative artwork on the right side of each main chart-selection row. The banner fades in from left to right behind the row's foreground content.

## Scope

- Apply the feature to `ChartListItemView`, which backs the main chart recycler in `MainMenuScene`.
- Keep the existing `#STAGEFILE` jacket and frame on the left unchanged.
- Reuse the existing `bms_parser::ChartMeta::Banner`, `Folder`, and asynchronous `ImageView` loading path. Parsing and repository persistence already support `#BANNER` and require no changes.
- Do not add banners to music-player, IR-upload, result, replay, folder, or modal list rows.
- Compile and commit Metal, SPIR-V, and GLES shader binaries in this change. DirectX shader compilation is intentionally deferred for the user to perform manually on Windows.

## Selected Approach

Add an optional directional fade style to the common `ImageView` component and apply it in a dedicated fragment shader. The shader samples the original image once and multiplies its alpha by a directional fade multiplier. Decoded RGBA data and the shared image cache remain untouched.

The shader approach keeps the effect at render time, avoids per-image pixel copies, and allows the same cached texture to be drawn normally or with different fade settings by different `ImageView` instances.

## Fade Contract

The common component exposes four directions:

- Left to right
- Right to left
- Top to bottom
- Bottom to top

It also accepts a strength clamped to `[0.0, 1.0]`:

- `0.0` leaves source alpha unchanged across the image.
- `1.0` makes the direction's origin transparent and reaches the source alpha at its destination.
- Intermediate strengths preserve `1 - strength` of the source alpha at the origin and interpolate to the full source alpha at the destination.

The chart banner uses left-to-right direction with strength `1.0`.

## Shader Design

Create `fs_image_fade.sc` using the existing `vs_text` varying contract and `s_texColor` sampler. A single `u_imageFadeParams` vector carries:

- `x`, `y`: the signed UV direction vector.
- `z`: the progress offset used by reverse directions.
- `w`: the fade strength.

The CPU maps directions as follows:

| Direction | x | y | offset |
| --- | ---: | ---: | ---: |
| Left to right | 1 | 0 | 0 |
| Right to left | -1 | 0 | 1 |
| Top to bottom | 0 | 1 | 0 |
| Bottom to top | 0 | -1 | 1 |

The fragment shader computes clamped progress from `dot(v_texcoord0, direction) + offset`, then computes `mix(1.0 - strength, 1.0, progress)`. It multiplies only sampled alpha by that value and preserves RGB.

## `ImageView` Component

Add a public property setter that accepts direction and strength, plus a clear method. `ImageView` stores the clamped configuration. When the property is present, rendering selects `vs_text.bin` plus `fs_image_fade.bin`, sets `u_imageFadeParams`, and submits the existing rounded image geometry. Without the property it continues using the current `fs_text.bin` program.

Changing or clearing the property requires no image reload because the fade is applied at draw time. Synchronous, cached, thumbnail, archive-member, and asynchronous images all use the same path.

## Chart-row Banner Layer

Each `ChartListItemView` owns a named `ImageView` banner layer inserted as the first child of the row's `contentCard`. It is absolutely positioned against the right edge, inset inside the one-pixel border, sized to the card's inner height and a fixed width close to the common 300:80 BMS banner aspect ratio. Because it is inserted before all normal row children, the clear lamp, jacket, text, score rank, difficulty, and favorite control render above it.

When `setMeta` binds a normal available chart with a non-empty `meta.Banner`, it requests `meta.Folder / meta.Banner` through `setImageAsync`. When the row is rebound to a chart without a banner, an unavailable chart, or a solid archive placeholder, it calls `freeImage` so neither an old texture nor its image identity can leak from the recycled row.

A failed or missing image leaves the layer empty without affecting row interaction or metadata. The banner is decorative and does not participate in flex layout or event handling.

## Testing

Add focused tests for:

1. Direction-to-uniform mapping and strength clamping for all four directions.
2. `ImageView` storing, clamping, changing, and clearing its fade property.
3. A shader audit proving the fragment shader samples the image, consumes the direction/offset/strength uniform, preserves RGB, and applies the fade to alpha.
4. `ChartListItemView` binding `Folder / Banner`, anchoring the banner on the right as the first background layer, configuring left-to-right strength `1.0`, and clearing recycled banner identity for empty, unavailable, and solid-archive rows.

Compile shaders with the repository shader script and verify the new Metal, SPIR-V, and GLES binaries. Run the focused targets, complete CTest suite, and desktop `main` build.
