# Ranking Modal Dismissal Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make both Bokutachi ranking overlays ignore outside touches while retaining blocked background interaction, and replace both text close buttons with compact Font Awesome xmark controls.

**Architecture:** Keep the behavior local to `IrRankingModalView.cpp`: simplify its private scrim so only Escape and Android Back request dismissal, and add a private icon-button factory using the existing Font Awesome utilities. Extend the existing ranking-detail source audit to lock down both dismissal policy and close-control construction without introducing production-only test hooks.

**Tech Stack:** C++23, SDL2 events, Yoga-based views, Font Awesome Solid, Python 3 source-audit tests, CMake/CTest

## Global Constraints

- Apply the behavior to both the main ranking modal and its score-detail modal.
- Outside mouse and touch input must not close either modal.
- Input outside the modal must remain blocked so it cannot activate controls in the underlying scene.
- Escape and Android Back must continue to close the currently active modal.
- Use Font Awesome Solid's xmark glyph (`0xf00d`) in a 48-by-48-pixel square button.
- The main xmark must invoke the existing ranking-modal close request; the detail xmark must invoke the existing score-detail hide action.
- Refresh, retry, row selection, and all other ranking behavior remain unchanged.
- Do not introduce a shared modal component or refactor unrelated action buttons.

---

### Task 1: Explicit-Only Ranking Modal Dismissal and Xmark Controls

**Files:**
- Modify: `scripts/check_ir_ranking_detail_flow.py`
- Modify: `src/ir/IrRankingModalView.cpp:1-145`
- Modify: `src/ir/IrRankingModalView.cpp:319-459`

**Interfaces:**
- Consumes: `ui_icons::kFontAwesomeSolidPath`, `ui_icons::textForCodepoint(uint32_t)`, `Button::setOnClickListener(std::function<void()>)`, and the existing `requestClose()` / `hideScoreDetails()` callbacks.
- Produces: private `makeIconActionButton(uint32_t codepoint, std::function<void()> action) -> Button *` and private `ModalScrim(std::function<void()> requestClose)` behavior that dismisses only for `SDLK_ESCAPE` or `SDLK_AC_BACK`.

- [ ] **Step 1: Write the failing source-audit regression checks**

Append these checks after the existing score-detail flow assertions in `scripts/check_ir_ranking_detail_flow.py`:

```python
require(
    "bool eventPoint(" not in source
    and "panel_->getX()" not in source
    and "panel_->getY()" not in source,
    "ranking modals must not dismiss from outside pointer hit testing",
)
require(
    source.count("new ModalScrim([this]()") == 2,
    "both ranking scrims must use explicit-only dismissal callbacks",
)
require(
    '#include "../view/IconText.h"' in source
    and "constexpr uint32_t kIconXmark = 0xf00d;" in source
    and source.count("makeIconActionButton(kIconXmark,") == 2
    and 'makeActionButton("Close"' not in source,
    "both ranking modal headers must use Font Awesome xmark buttons",
)
```

- [ ] **Step 2: Run the audit and confirm the new checks fail**

Run:

```bash
python3 scripts/check_ir_ranking_detail_flow.py .
```

Expected: exit status 1 with failures including `ranking modals must not dismiss from outside pointer hit testing`, `both ranking scrims must use explicit-only dismissal callbacks`, and `both ranking modal headers must use Font Awesome xmark buttons`.

- [ ] **Step 3: Add the Font Awesome xmark button factory**

Add the existing icon utility include beside the other view includes:

```cpp
#include "../view/IconText.h"
```

Add the xmark constant beside the existing view constants:

```cpp
constexpr uint32_t kIconXmark = 0xf00d;
```

Add this factory immediately after `makeActionButton`:

```cpp
Button *makeIconActionButton(uint32_t codepoint,
                             std::function<void()> action) {
  auto *button = new Button();
  auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, 22);
  icon->setText(ui_icons::textForCodepoint(codepoint));
  icon->setAlign(TextView::CENTER);
  icon->setVAlign(TextView::MIDDLE);
  icon->setThemedColor(ui_theme::textPrimary);
  button->setContentView(icon);
  button->setWidth(48)->setHeight(48)->setFlexShrink(0);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  button->setThemedBorderColors(ui_theme::hairlineStrong,
                                ui_theme::accentBorderStrong,
                                ui_theme::accentBorderStrong);
  button->setStyledBorderWidth(1);
  button->setOnClickListener(std::move(action));
  return button;
}
```

- [ ] **Step 4: Remove pointer-coordinate dismissal from the modal scrim**

Delete the complete `eventPoint(const SDL_Event &, float &, float &)` helper, then replace `ModalScrim` with:

```cpp
class ModalScrim final : public View {
public:
  explicit ModalScrim(std::function<void()> requestClose)
      : requestClose_(std::move(requestClose)) {}

private:
  bool handleEventsImpl(SDL_Event &event) override {
    if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
        (event.key.keysym.sym == SDLK_ESCAPE ||
         event.key.keysym.sym == SDLK_AC_BACK)) {
      requestClose_();
    }
    return false;
  }

  std::function<void()> requestClose_;
};
```

Returning `false` for every event preserves the existing overlay input barrier, while removing panel hit testing ensures outside pointer events do not dismiss either modal.

- [ ] **Step 5: Wire both scrims and both header controls to the explicit-only behavior**

Construct the main ranking scrim without its panel argument:

```cpp
root = new ModalScrim([this]() { requestClose(); });
```

Replace the main header's text close button with:

```cpp
closeButton =
    makeIconActionButton(kIconXmark, [this]() { requestClose(); });
```

Construct the score-detail scrim without its panel argument:

```cpp
scoreDetailRoot =
    new ModalScrim([this]() { hideScoreDetails(); });
```

Replace the score-detail header's text close button with:

```cpp
detailHeader->addView(
    makeIconActionButton(kIconXmark, [this]() { hideScoreDetails(); }));
```

- [ ] **Step 6: Run the focused regression checks**

Run:

```bash
python3 scripts/check_ir_ranking_detail_flow.py .
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_ranking_detail_flow_audit|ir_ranking_modal_tests)$'
```

Expected: the Python audit prints `IR ranking score-detail flow audit passed`; CTest reports both selected tests passed with zero failures.

- [ ] **Step 7: Build the desktop application target**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: the `main` target reaches 100% and exits successfully with no compile or link errors.

- [ ] **Step 8: Review and commit the implementation**

Run:

```bash
git diff --check
git diff -- scripts/check_ir_ranking_detail_flow.py src/ir/IrRankingModalView.cpp
git status --short
git add scripts/check_ir_ranking_detail_flow.py src/ir/IrRankingModalView.cpp
git commit -m "fix: require explicit ranking modal dismissal"
```

Expected: the diff contains only the audit and ranking-view changes described above, and the commit succeeds.
