# Chart List Banner Background Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render each available chart's `#BANNER` image behind the right side of the main chart-list row using a configurable directional fade shader.

**Architecture:** A small value type maps four fade directions plus clamped strength to a shader uniform. `ImageView` selects a dedicated fragment shader when that optional property is set, while `ChartListItemView` binds a right-anchored banner layer before its foreground content.

**Tech Stack:** C++23, SDL2, bgfx shader language, Yoga, CMake/CTest

## Global Constraints

- Apply banners only to `ChartListItemView` in the main chart recycler.
- Keep the existing left-side `#STAGEFILE` jacket unchanged.
- Use the existing parsed and persisted `bms_parser::ChartMeta::Banner`; do not edit the amalgamated BMS parser.
- The shader must accept direction and strength at render time and must not copy or modify cached RGBA pixels.
- Strength is clamped to `[0.0, 1.0]`; `0.0` means unchanged alpha and `1.0` means transparent at the origin through source alpha at the destination.
- Support left-to-right, right-to-left, top-to-bottom, and bottom-to-top directions.
- Chart-row banners use left-to-right direction at strength `1.0`.
- Compile and commit Metal, SPIR-V, and GLES shader binaries. Do not create a DirectX binary in this change; the user will compile it manually later.
- Clear recycled banner identity for missing banners, unavailable charts, and solid archives.

---

### Task 1: Direction and strength model

**Files:**
- Create: `src/view/ImageFade.h`
- Create: `src/view/ImageFade.cpp`
- Create: `tests/image_fade_tests.cpp`
- Modify: `src/view/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ImageFadeDirection`, `ImageFade`, `makeImageFade(ImageFadeDirection, float)`, and `imageFadeShaderParameters(const ImageFade &) -> std::array<float, 4>`.
- Consumes: a direction enum and arbitrary floating-point strength.

- [ ] **Step 1: Write the failing model test**

Create `tests/image_fade_tests.cpp` with assertions that:

```cpp
const auto leftToRight = makeImageFade(ImageFadeDirection::LeftToRight, 2.0F);
require(leftToRight.strength == 1.0F, "fade strength clamps high values");
require(imageFadeShaderParameters(leftToRight) ==
            std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F},
        "left-to-right maps x progress and clamped strength");

const auto rightToLeft =
    makeImageFade(ImageFadeDirection::RightToLeft, 0.75F);
require(imageFadeShaderParameters(rightToLeft) ==
            std::array<float, 4>{-1.0F, 0.0F, 1.0F, 0.75F},
        "right-to-left reverses x progress");

const auto topToBottom =
    makeImageFade(ImageFadeDirection::TopToBottom, -1.0F);
require(imageFadeShaderParameters(topToBottom) ==
            std::array<float, 4>{0.0F, 1.0F, 0.0F, 0.0F},
        "top-to-bottom maps y progress and clamps low strength");

const auto bottomToTop =
    makeImageFade(ImageFadeDirection::BottomToTop, 0.5F);
require(imageFadeShaderParameters(bottomToTop) ==
            std::array<float, 4>{0.0F, -1.0F, 1.0F, 0.5F},
        "bottom-to-top reverses y progress");
```

Register `image_fade_tests` from `tests/image_fade_tests.cpp` and `src/view/ImageFade.cpp`, add it to the standard `asobmashow_register_test` loop, and add `ImageFade.cpp` to `src/view/CMakeLists.txt`.

- [ ] **Step 2: Run the test to verify RED**

```bash
cmake --build cmake-build-debug --target image_fade_tests -j 6
```

Expected: compilation fails because `view/ImageFade.h` does not exist.

- [ ] **Step 3: Implement the minimal model**

Declare the enum, value struct, and functions in `ImageFade.h`. Implement `makeImageFade` with `std::clamp(strength, 0.0F, 1.0F)`. Implement the four exact direction mappings shown by the tests in `ImageFade.cpp`.

- [ ] **Step 4: Run the test to verify GREEN**

```bash
cmake --build cmake-build-debug --target image_fade_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^image_fade_tests$'
```

Expected: one test passes.

- [ ] **Step 5: Commit the model**

```bash
git add CMakeLists.txt src/view/CMakeLists.txt src/view/ImageFade.h src/view/ImageFade.cpp tests/image_fade_tests.cpp
git commit -m "feat: add directional image fade model"
```

### Task 2: Directional fade fragment shader

**Files:**
- Create: `scripts/check_image_fade_shader.py`
- Create: `shader_src/fs_image_fade.sc`
- Create by shader compiler: `shaders/metal/fs_image_fade.bin`
- Create by shader compiler: `shaders/spirv/fs_image_fade.bin`
- Create by shader compiler: `shaders/essl/fs_image_fade.bin`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `v_texcoord0`, `s_texColor`, and `u_imageFadeParams = {directionX, directionY, offset, strength}`.
- Produces: sampled RGB unchanged and sampled alpha multiplied by the configured directional fade.

- [ ] **Step 1: Write the failing shader contract audit**

Create `scripts/check_image_fade_shader.py` to load `shader_src/fs_image_fade.sc` and fail unless all of these source fragments exist:

```python
required = {
    "fade uniform": "uniform vec4 u_imageFadeParams",
    "image sampler": "SAMPLER2D(s_texColor, 0)",
    "direction progress": "dot(v_texcoord0, u_imageFadeParams.xy)",
    "offset progress": "+ u_imageFadeParams.z",
    "strength clamp": "saturate(u_imageFadeParams.w)",
    "alpha-only fade": "color.a *= alphaMultiplier",
    "preserved color output": "gl_FragColor = color",
}
```

Register it as `image_fade_shader_audit` beside the other Python audits in `CMakeLists.txt`.

- [ ] **Step 2: Run the audit to verify RED**

```bash
python3 scripts/check_image_fade_shader.py .
```

Expected: failure reports the missing `shader_src/fs_image_fade.sc` contract.

- [ ] **Step 3: Implement the shader**

Create `shader_src/fs_image_fade.sc`:

```glsl
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_imageFadeParams;

void main() {
    vec4 color = texture2D(s_texColor, v_texcoord0);
    float progress = saturate(
        dot(v_texcoord0, u_imageFadeParams.xy) + u_imageFadeParams.z);
    float strength = saturate(u_imageFadeParams.w);
    float alphaMultiplier = mix(1.0 - strength, 1.0, progress);
    color.a *= alphaMultiplier;
    gl_FragColor = color;
}
```

- [ ] **Step 4: Verify the audit and compile supported binaries**

```bash
python3 scripts/check_image_fade_shader.py .
cd shader_src
SHADERC=../bgfx/bgfx/.build/osx-arm64/bin/shadercRelease python3 make.py
cd ..
test -s shaders/metal/fs_image_fade.bin
test -s shaders/spirv/fs_image_fade.bin
test -s shaders/essl/fs_image_fade.bin
test ! -e shaders/dx11/fs_image_fade.bin
```

Expected: the audit passes; Metal, SPIR-V, and GLES binaries exist; no DirectX binary is created.

- [ ] **Step 5: Commit the shader**

```bash
git add CMakeLists.txt scripts/check_image_fade_shader.py shader_src/fs_image_fade.sc shaders/metal/fs_image_fade.bin shaders/spirv/fs_image_fade.bin shaders/essl/fs_image_fade.bin
git commit -m "feat: add directional image fade shader"
```

### Task 3: Reusable `ImageView` fade property

**Files:**
- Create: `tests/image_view_fade_tests.cpp`
- Modify: `src/view/ImageView.h`
- Modify: `src/view/ImageView.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `makeImageFade`, `imageFadeShaderParameters`, `fs_image_fade.bin`, and `u_imageFadeParams`.
- Produces: `ImageView::setFade(ImageFadeDirection, float)`, `clearFade()`, and `fade() const`.

- [ ] **Step 1: Write the failing property test**

Create a headless Noop-bgfx test that constructs `ImageView`, calls `setFade(ImageFadeDirection::RightToLeft, 2.0F)`, and asserts its optional `fade()` contains the direction with strength `1.0F`. Change it to `TopToBottom, 0.25F`, verify the replacement, call `clearFade()`, and verify the optional is empty. Initialize the rendering globals with the same pattern used by `tests/ir_upload_candidate_list_view_tests.cpp`.

Register the target with `ImageFade.cpp`, `ImageView.cpp`, `View.cpp`, rendering support, `ArchiveFile.cpp`, `MinizBridge.c`, and `path.cpp`; link `${COMMON_LIBS} bgfx yogacore` and `iconv` on Apple. Add it to the standard test-registration loop.

- [ ] **Step 2: Run the test to verify RED**

```bash
cmake --build cmake-build-debug --target image_view_fade_tests -j 6
```

Expected: compilation fails because the `ImageView` fade property API does not exist.

- [ ] **Step 3: Implement shader selection and uniform binding**

Store `std::optional<ImageFade> fade_` in `ImageView`. The setters use `makeImageFade` and require no texture reload. Refactor `submitTexturedRoundedRect` to receive the chosen program handle. In `renderImpl`:

```cpp
bgfx::ProgramHandle program;
if (fade_.has_value()) {
  const auto params = imageFadeShaderParameters(*fade_);
  bgfx::setUniform(
      rendering::UniformCache::getInstance().getVec4("u_imageFadeParams"),
      params.data());
  program = rendering::ShaderManager::getInstance().getProgram(
      "vs_text.bin", "fs_image_fade.bin");
} else {
  program = rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
}
```

Submit the existing rounded geometry with `program`. Do not change decoding, async loading, cache keys, cached RGBA, or texture creation.

- [ ] **Step 4: Run focused tests to verify GREEN**

```bash
cmake --build cmake-build-debug --target image_fade_tests image_view_fade_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(image_fade_tests|image_view_fade_tests|image_fade_shader_audit)$'
```

Expected: all three focused tests pass.

- [ ] **Step 5: Commit the common component change**

```bash
git add CMakeLists.txt src/view/ImageView.h src/view/ImageView.cpp tests/image_view_fade_tests.cpp
git commit -m "feat: add image view directional fade"
```

### Task 4: Right-side chart banner background

**Files:**
- Create: `tests/chart_list_item_view_tests.cpp`
- Modify: `src/view/ChartListItemView.h`
- Modify: `src/view/ChartListItemView.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ChartMetaRecord::meta.Folder`, `ChartMetaRecord::meta.Banner`, and `ImageView::setFade(ImageFadeDirection::LeftToRight, 1.0F)`.
- Produces: a named `chartListBanner` background `ImageView` owned by each `ChartListItemView`.

- [ ] **Step 1: Write the failing chart-row test**

Create a Noop-bgfx test using a `1200 x 108` row. Bind `Folder = "/charts/first"` and `Banner = "banner.png"`, then assert the named card and banner exist, the image path is `/charts/first/banner.png`, the fade is left-to-right at strength `1.0F`, the banner's right edge is inset one pixel from the card, and the banner is the card's first child. Rebind the same row to an empty banner, an unavailable record, and a solid archive; each must leave `imagePath().empty()`.

Register `chart_list_item_view_tests` with `ChartListItemView.cpp`, `ImageFade.cpp`, `ImageView.cpp`, `Button.cpp`, `TextView.cpp`, `View.cpp`, rendering support, `ArchiveFile.cpp`, `MinizBridge.c`, `bms_parser.cpp`, and `path.cpp`. Link `${COMMON_LIBS} bgfx yogacore` and `iconv` on Apple. Add it to the standard test-registration loop.

- [ ] **Step 2: Run the test to verify RED**

```bash
cmake --build cmake-build-debug --target chart_list_item_view_tests -j 6
```

Expected: the test fails because the named banner layer and binding do not exist.

- [ ] **Step 3: Implement banner layout and binding**

Add `ImageView *bannerImage` to `ChartListItemView`. Name `contentCard` as `chartListContentCard`. Before adding `clearLamp`, create `chartListBanner` as an absolute first child, set top/right to `1`, width to `368`, height to `height - kBottomGap - 2`, the inset child corner radius, and left-to-right fade strength `1.0F`.

In `setMeta`, bind `meta.Folder / meta.Banner` only when the row is not unavailable, not a solid archive, and the banner is non-empty. Otherwise call `bannerImage->freeImage()`.

- [ ] **Step 4: Run focused tests to verify GREEN**

```bash
cmake --build cmake-build-debug --target image_fade_tests image_view_fade_tests chart_list_item_view_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(image_fade_tests|image_view_fade_tests|image_fade_shader_audit|chart_list_item_view_tests)$'
```

Expected: all four focused tests pass.

- [ ] **Step 5: Commit chart-row rendering**

```bash
git add CMakeLists.txt src/view/ChartListItemView.h src/view/ChartListItemView.cpp tests/chart_list_item_view_tests.cpp
git commit -m "feat: render banners in chart list rows"
```

### Task 5: Regression verification

**Files:**
- Verify only; no planned source changes.

**Interfaces:**
- Consumes: all deliverables from Tasks 1-4.
- Produces: fresh build and test evidence.

- [ ] **Step 1: Run the complete test suite**

```bash
cmake --build cmake-build-debug -j 6
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: every configured test passes.

- [ ] **Step 2: Run the desktop build check**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: `main` links successfully and packages the Metal shader used on this machine.

- [ ] **Step 3: Inspect final repository state**

```bash
git diff --check
git status -sb
git log --oneline -8
```

Expected: no whitespace errors or unintended files; the DirectX fade binary remains intentionally absent.
