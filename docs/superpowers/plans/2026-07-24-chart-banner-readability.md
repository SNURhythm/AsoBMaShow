# Chart Banner Readability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve the readability of chart-row UI over arbitrary `#BANNER` artwork with a reusable, theme-aware shader scrim.

**Architecture:** `ImageView` gains independent optional fade and scrim state, including a themed scrim provider that is reevaluated during theme propagation. The existing image fade shader accepts a second vec4 uniform and blends sampled RGB toward the scrim color before preserving the current directional alpha fade. `ChartListItemView` configures exact dark- and light-theme scrim colors on its banner image.

**Tech Stack:** C++23, bgfx uniforms and shaderc, bgfx shader language, Yoga UI layout, CTest.

## Global Constraints

- Preserve the existing `u_imageFadeParams` direction, offset, and strength contract.
- Use `Color(5, 10, 18, 144)` in dark theme and `Color(255, 255, 255, 168)` in light theme.
- Regular `ImageView` instances have neither fade nor scrim by default.
- Do not add or compile a Windows/DirectX shader binary.
- Regenerate only Metal, SPIR-V, and GLES shader binaries.
- Keep the banner as the chart content card's first child behind all row UI.

---

### Task 1: Add reusable themed scrim state to `ImageView`

**Files:**
- Modify: `tests/image_view_fade_tests.cpp`
- Modify: `src/view/ImageView.h`
- Modify: `src/view/ImageView.cpp`

**Interfaces:**
- Consumes: `View::ThemeColorProvider`, `View::onThemeChanged()`, and `Color`.
- Produces: `ImageView *setScrimColor(const Color &)`, `ImageView *setThemedScrimColor(ThemeColorProvider)`, `ImageView *clearScrimColor()`, and `const std::optional<Color> &scrimColor() const noexcept`.

- [ ] **Step 1: Write the failing fixed/themed scrim state test**

Add channel comparison helpers and extend the existing `ImageView` test:

```cpp
bool sameColor(const Color &actual, const Color &expected) {
  return actual.r == expected.r && actual.g == expected.g &&
         actual.b == expected.b && actual.a == expected.a;
}

require(!image.scrimColor().has_value(),
        "image starts without a readability scrim");
image.setScrimColor(Color(3, 4, 5, 96));
require(image.scrimColor().has_value() &&
            sameColor(*image.scrimColor(), Color(3, 4, 5, 96)),
        "image stores a fixed scrim independently of fade state");

bool useLightScrim = false;
image.setThemedScrimColor([&useLightScrim] {
  return useLightScrim ? Color(255, 255, 255, 168)
                       : Color(5, 10, 18, 144);
});
require(sameColor(*image.scrimColor(), Color(5, 10, 18, 144)),
        "themed scrim evaluates immediately");
useLightScrim = true;
image.propagateThemeChange();
require(sameColor(*image.scrimColor(), Color(255, 255, 255, 168)),
        "themed scrim reevaluates during theme propagation");
image.clearScrimColor();
require(!image.scrimColor().has_value(),
        "clearing scrim restores untreated image color");
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
cmake --build cmake-build-debug --target image_view_fade_tests -j 6
```

Expected: compilation fails because the scrim API does not exist.

- [ ] **Step 3: Add the scrim API and theme lifecycle**

Add to `ImageView`:

```cpp
private:
  std::optional<Color> scrimColor_;
  ThemeColorProvider themedScrimColorProvider_;

protected:
  void onThemeChanged() override;

public:
  ImageView *setScrimColor(const Color &color);
  ImageView *setThemedScrimColor(ThemeColorProvider provider);
  ImageView *clearScrimColor();
  [[nodiscard]] const std::optional<Color> &scrimColor() const noexcept {
    return scrimColor_;
  }
```

Implement it in `ImageView.cpp`:

```cpp
ImageView *ImageView::setScrimColor(const Color &color) {
  themedScrimColorProvider_ = {};
  scrimColor_ = color;
  return this;
}

ImageView *ImageView::setThemedScrimColor(ThemeColorProvider provider) {
  themedScrimColorProvider_ = std::move(provider);
  scrimColor_ = themedScrimColorProvider_ ?
      std::optional<Color>(themedScrimColorProvider_()) : std::nullopt;
  return this;
}

ImageView *ImageView::clearScrimColor() {
  themedScrimColorProvider_ = {};
  scrimColor_.reset();
  return this;
}

void ImageView::onThemeChanged() {
  View::onThemeChanged();
  if (themedScrimColorProvider_) {
    scrimColor_ = themedScrimColorProvider_();
  }
}
```

Include `<utility>` if required for `std::move`.

- [ ] **Step 4: Run the focused state test**

Run:

```bash
cmake --build cmake-build-debug --target image_view_fade_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^image_view_fade_tests$'
```

Expected: build succeeds and 1/1 test passes.

- [ ] **Step 5: Commit the reusable state API**

```bash
git add src/view/ImageView.h src/view/ImageView.cpp tests/image_view_fade_tests.cpp
git commit -m "feat: add themed image scrim state"
```

### Task 2: Apply the scrim in the image fade shader

**Files:**
- Modify: `scripts/check_image_fade_shader.py`
- Modify: `shader_src/fs_image_fade.sc`
- Modify: `src/view/ImageView.cpp`
- Regenerate: `shaders/metal/fs_image_fade.bin`
- Regenerate: `shaders/spirv/fs_image_fade.bin`
- Regenerate: `shaders/essl/fs_image_fade.bin`

**Interfaces:**
- Consumes: `ImageView::scrimColor_`, `ImageView::fade_`, `imageFadeShaderParameters()`, and `rendering::UniformCache::getVec4()`.
- Produces: the `u_imageScrimColor` uniform contract of normalized RGBA values.

- [ ] **Step 1: Strengthen the shader audit before changing the shader**

Add these required fragments to `scripts/check_image_fade_shader.py`:

```python
"scrim uniform": "uniform vec4 u_imageScrimColor",
"scrim alpha clamp": "saturate(u_imageScrimColor.a)",
"scrim rgb blend": "mix(color.rgb, u_imageScrimColor.rgb, scrimAlpha)",
```

Keep the existing direction, strength, alpha fade, and final output requirements.

- [ ] **Step 2: Run the audit to verify it fails**

Run:

```bash
python3 scripts/check_image_fade_shader.py
```

Expected: failure reports the missing scrim uniform, alpha clamp, and RGB blend.

- [ ] **Step 3: Add the shader scrim blend**

Update `shader_src/fs_image_fade.sc`:

```glsl
SAMPLER2D(s_texColor, 0);
uniform vec4 u_imageFadeParams;
uniform vec4 u_imageScrimColor;

void main() {
    vec4 color = texture2D(s_texColor, v_texcoord0);
    float scrimAlpha = saturate(u_imageScrimColor.a);
    color.rgb = mix(color.rgb, u_imageScrimColor.rgb, scrimAlpha);
    float progress = saturate(
        dot(v_texcoord0, u_imageFadeParams.xy) + u_imageFadeParams.z);
    float strength = saturate(u_imageFadeParams.w);
    float alphaMultiplier = mix(1.0 - strength, 1.0, progress);
    color.a *= alphaMultiplier;
    gl_FragColor = color;
}
```

- [ ] **Step 4: Bind neutral defaults and normalized scrim RGBA**

In `ImageView::renderImpl()`, select the specialized shader when either treatment exists, supply zero-strength left-to-right fade parameters when no fade exists, and bind normalized RGBA:

```cpp
if (fade_.has_value() || scrimColor_.has_value()) {
  const ImageFade fade = fade_.value_or(
      makeImageFade(ImageFadeDirection::LeftToRight, 0.0F));
  const auto fadeParams = imageFadeShaderParameters(fade);
  bgfx::setUniform(
      rendering::UniformCache::getInstance().getVec4("u_imageFadeParams"),
      fadeParams.data());

  const Color scrim = scrimColor_.value_or(Color(0, 0, 0, 0));
  constexpr float inv255 = 1.0F / 255.0F;
  const std::array<float, 4> scrimParams = {
      scrim.r * inv255, scrim.g * inv255,
      scrim.b * inv255, scrim.a * inv255};
  bgfx::setUniform(
      rendering::UniformCache::getInstance().getVec4("u_imageScrimColor"),
      scrimParams.data());
  program = rendering::ShaderManager::getInstance().getProgram(
      "vs_text.bin", "fs_image_fade.bin");
} else {
  program = rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
}
```

- [ ] **Step 5: Audit and compile supported shader binaries**

Run:

```bash
python3 scripts/check_image_fade_shader.py
cd shader_src && SHADERC=../bgfx/bgfx/.build/osx-arm64/bin/shadercRelease python3 make.py
```

Expected: audit passes; Metal, SPIR-V, and GLES `fs_image_fade.bin` files are regenerated. No `shaders/dx11/fs_image_fade.bin` is created.

- [ ] **Step 6: Build and run the common image test**

Run:

```bash
cmake --build cmake-build-debug --target image_view_fade_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'image_fade_shader_audit|image_view_fade_tests'
```

Expected: 2/2 tests pass.

- [ ] **Step 7: Commit shader rendering support**

```bash
git add scripts/check_image_fade_shader.py shader_src/fs_image_fade.sc \
  src/view/ImageView.cpp shaders/metal/fs_image_fade.bin \
  shaders/spirv/fs_image_fade.bin shaders/essl/fs_image_fade.bin
git commit -m "feat: add image shader readability scrim"
```

### Task 3: Configure and verify the chart banner scrim

**Files:**
- Modify: `tests/chart_list_item_view_tests.cpp`
- Modify: `src/view/ChartListItemView.cpp`

**Interfaces:**
- Consumes: `ImageView::setThemedScrimColor()`, `ImageView::scrimColor()`, and `ui_theme::activeMode()`.
- Produces: exact theme-aware readability treatment for `chartListBanner`.

- [ ] **Step 1: Write the failing chart-row theme test**

Include `view/UiTheme.h`, add the same channel comparison helper, force the dark theme before constructing the row, and assert both theme values:

```cpp
ui_theme::setActiveMode(ui_theme::ThemeMode::Dark);
ChartListItemView row(0, 0, 1200, 108, record);
row.setMeta(record);

require(banner->scrimColor().has_value() &&
            sameColor(*banner->scrimColor(), Color(5, 10, 18, 144)),
        "dark chart banner uses the readability scrim");
ui_theme::setActiveMode(ui_theme::ThemeMode::Light);
row.propagateThemeChange();
require(banner->scrimColor().has_value() &&
            sameColor(*banner->scrimColor(),
                      Color(255, 255, 255, 168)),
        "chart banner scrim follows the active light theme");
ui_theme::setActiveMode(ui_theme::ThemeMode::Dark);
row.propagateThemeChange();
```

- [ ] **Step 2: Run the chart-row test to verify it fails**

Run:

```bash
cmake --build cmake-build-debug --target chart_list_item_view_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^chart_list_item_view_tests$'
```

Expected: test fails because the chart banner has no scrim.

- [ ] **Step 3: Configure the themed banner scrim**

After configuring the banner fade in `ChartListItemView`:

```cpp
bannerImage->setThemedScrimColor([] {
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? Color(255, 255, 255, 168)
             : Color(5, 10, 18, 144);
});
```

- [ ] **Step 4: Run the focused banner suite**

Run:

```bash
cmake --build cmake-build-debug --target chart_list_item_view_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R 'image_fade_shader_audit|image_fade_tests|image_view_fade_tests|chart_list_item_view_tests'
```

Expected: 4/4 tests pass.

- [ ] **Step 5: Commit chart-row configuration**

```bash
git add src/view/ChartListItemView.cpp tests/chart_list_item_view_tests.cpp
git commit -m "feat: improve chart banner UI readability"
```

### Task 4: Verify the completed branch

**Files:**
- Verify only; no expected source changes.

**Interfaces:**
- Consumes: all prior task outputs.
- Produces: fresh build, test, shader, and repository-hygiene evidence.

- [ ] **Step 1: Run the shader contract and platform-output checks**

```bash
python3 scripts/check_image_fade_shader.py
test -f shaders/metal/fs_image_fade.bin
test -f shaders/spirv/fs_image_fade.bin
test -f shaders/essl/fs_image_fade.bin
test ! -e shaders/dx11/fs_image_fade.bin
```

Expected: all commands exit 0.

- [ ] **Step 2: Build all configured targets and the desktop app explicitly**

```bash
cmake --build cmake-build-debug -j 6
cmake --build cmake-build-debug --target main -j 6
```

Expected: both builds exit 0.

- [ ] **Step 3: Run the full test suite**

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: 140/140 tests pass.

- [ ] **Step 4: Check repository hygiene**

```bash
git diff --check
git status --short
```

Expected: no whitespace errors and an empty worktree status.

