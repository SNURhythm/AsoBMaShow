#include "ColorPickerPopup.h"
#include "ColorPickerView.h"
#include "ModalViewHelpers.h"
#include "../settings/BuiltInNotes.h"
#include "../settings/BuiltInScratchGradient.h"

namespace {
void place(View *view, int x, int y, int width, int height) {
  view->setPosition(x, y, YGPositionTypeAbsolute);
  view->setSize(std::max(0, width), std::max(0, height));
}
TextView *text(const i18n::Text &label, int size) {
  auto *view = new TextView("assets/fonts/notosanscjkjp.ttf", size);
  view->setLocalizedText(label);
  view->setThemedColor(ui_theme::textPrimary);
  view->setAlign(TextView::CENTER);
  view->setVAlign(TextView::MIDDLE);
  view->setAutoFitText(true);
  return view;
}
} // namespace

class ColorPickerPopup::SampleView final : public View {
public:
  explicit SampleView(Sample sample) : sample(std::move(sample)) {}
  void setColor(std::uint32_t value) {
    color = value;
    setBackgroundColor(sample.style == SampleStyle::Outline ? Color(0, 0, 0, 0)
        : Color((std::uint32_t(sample.alpha) << 24U) | color));
    if (sample.style == SampleStyle::Outline) {
      setBorderColor(Color(0xE0000000U | color));
      setBorderWidth(std::max(1, int(getHeight() * 0.15F)));
    }
  }
private:
  Sample sample;
  std::uint32_t color = 0;
  void renderImpl(RenderContext &context) override {
    if (sample.style != SampleStyle::Scratch) return;
    const auto stops = built_in_notes::scratchGradient(color);
    const auto program = rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
    const auto state = context.makeUiBatchState(program, BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
    constexpr std::array<std::uint16_t, 6> indices{0, 1, 2, 2, 3, 0};
    for (std::size_t i = 1; i < stops.size(); ++i) {
      const float x0 = getX() + getWidth() * stops[i - 1].position;
      const float x1 = getX() + getWidth() * stops[i].position;
      const float y0 = getY(), y1 = getY() + getHeight();
      const auto left = built_in_notes::abgr(stops[i - 1].color);
      const auto right = built_in_notes::abgr(stops[i].color);
      const std::array vertices{rendering::PosColorVertex{x0, y0, 0, left},
          rendering::PosColorVertex{x1, y0, 0, right},
          rendering::PosColorVertex{x1, y1, 0, right},
          rendering::PosColorVertex{x0, y1, 0, left}};
      context.appendUiColor(vertices, indices, state);
    }
  }
};

ColorPickerPopup::ColorPickerPopup(color_picker::Hsv initial, std::vector<Sample> samples,
                                   std::function<void(std::uint32_t)> onPreviewColor)
    : draft(initial), onPreviewColor(std::move(onPreviewColor)), samples(std::move(samples)) {
  setThemedBackgroundColor(ui_theme::scrim);
  panel = new View();
  panel->setThemedBackgroundColor(ui_theme::panelStrong);
  panel->setCornerRadius(ui_theme::controlRadius());
  addView(panel);
  title = text(i18n::message("settings.color_picker.title"), 28);
  panel->addView(title);
  for (const auto &sample : this->samples) {
    auto *frame = new View();
    frame->setBackgroundColor(Color(20, 24, 30));
    panel->addView(frame);
    sampleFrames.push_back(frame);
    auto *label = text(i18n::Text(sample.label), 18);
    label->setThemedColor([] { return Color(230, 235, 240); });
    frame->addView(label);
    sampleLabels.push_back(label);
    auto *note = new SampleView(sample);
    frame->addView(note);
    sampleViews.push_back(note);
  }
  hex = text(i18n::Text(""), 22);
  panel->addView(hex);
  picker = new ColorPickerView(draft, [this](color_picker::Hsv color, bool) {
    draft = color;
    refreshDraft();
    if (this->onPreviewColor) this->onPreviewColor(color_picker::toRgb(draft));
  });
  panel->addView(picker);
  TextView *buttonText = nullptr;
  cancel = modal_view::makeModalButton(i18n::message("settings.color_picker.cancel"), 24, &buttonText);
  modal_view::styleThemedActionButton(cancel, buttonText, true, ui_theme::panelSubtle,
      ui_theme::panelStrong, ui_theme::panelSubtle, ui_theme::hairlineStrong);
  cancel->setOnClickListener([this] { outcome = Result{false, draft}; });
  panel->addView(cancel);
  confirm = modal_view::makeModalButton(i18n::message("settings.color_picker.confirm"), 24, &buttonText);
  modal_view::styleThemedActionButton(confirm, buttonText, true, ui_theme::primaryAction,
      ui_theme::primaryActionHover, ui_theme::primaryActionPressed, ui_theme::primaryAction);
  confirm->setOnClickListener([this] { outcome = Result{true, draft}; });
  panel->addView(confirm);
  refreshDraft();
}

void ColorPickerPopup::refreshDraft() {
  const auto rgb = color_picker::toRgb(draft);
  hex->setText("#" + built_in_notes::colorHex(rgb));
  for (auto *sample : sampleViews) sample->setColor(rgb);
}

void ColorPickerPopup::fitToViewport(int width, int height, int left, int top, int right, int bottom) {
  setSize(width, height);
  width = std::max(0, width - left - right);
  height = std::max(0, height - top - bottom);
  const int margin = std::min({24, width / 20, height / 20});
  const int w = std::min(1000, width - margin * 2);
  const int h = std::min(900, height - margin * 2);
  const int panelX = left + (width - w) / 2, panelY = top + (height - h) / 2;
  if (panel->getX() == panelX && panel->getY() == panelY &&
      panel->getWidth() == w && panel->getHeight() == h) return;
  place(panel, panelX, panelY, w, h);
  const int padding = std::min({24, w / 20, h / 40});
  const int inner = w - 2 * padding;
  const int gap = std::min(12, h / 60);
  const int titleHeight = std::min(42, h / 12);
  const int buttonHeight = std::min(58, h / 8);
  const int hexHeight = std::min(32, h / 14);
  // Keep at least 64 px of saturation/value space after the picker's 72 px
  // hue-strip/inset overhead, even on short landscape screens.
  const int previewBudget = std::max(0, h - 2 * padding - titleHeight - hexHeight -
      buttonHeight - 4 * gap - 136);
  const int previewHeight = samples.empty() ? 0 : std::min({210, h / 4, previewBudget});
  int y = padding;
  place(title, padding, y, inner, titleHeight);
  y += titleHeight + gap;
  const int gridGap = std::min(4, gap);
  const int maxRows = std::max(1, (previewHeight + gridGap) / 20);
  const int columns = std::max({1, std::min(int(samples.size()), inner / 96),
      (int(samples.size()) + maxRows - 1) / maxRows});
  const int rows = std::max(1, (int(samples.size()) + columns - 1) / columns);
  const int cellWidth = (inner - gridGap * (columns - 1)) / columns;
  const int cellHeight = (previewHeight - gridGap * (rows - 1)) / rows;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    place(sampleFrames[i], padding + int(i % columns) * (cellWidth + gridGap),
          y + int(i / columns) * (cellHeight + gridGap), cellWidth, cellHeight);
    const int labelHeight = std::min(24, cellHeight / 3);
    place(sampleLabels[i], 0, 0, cellWidth, labelHeight);
    const auto &sample = samples[i];
    const float scale = std::max(0.0F, std::min({1.0F,
        (cellWidth - 8.0F) / std::max(1.0F, sample.width),
        (cellHeight - labelHeight - 4.0F) / std::max(1.0F, sample.height)}));
    const int sw = int(sample.width * scale), sh = std::max(1, int(sample.height * scale));
    place(sampleViews[i], (cellWidth - sw) / 2,
          labelHeight + (cellHeight - labelHeight - sh) / 2, sw, sh);
  }
  y += previewHeight + gap;
  place(hex, padding, y, inner, hexHeight);
  y += hexHeight + gap;
  const int buttonsY = h - padding - buttonHeight;
  place(picker, padding, y, inner, buttonsY - gap - y);
  place(cancel, padding, buttonsY, (inner - gap) / 2, buttonHeight);
  place(confirm, padding + (inner + gap) / 2, buttonsY, (inner - gap) / 2, buttonHeight);
  applyYogaLayout();
  refreshDraft();
}

bool ColorPickerPopup::handleEventsImpl(SDL_Event &event) {
  if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
    outcome = Result{false, draft};
  switch (event.type) {
  case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION:
  case SDL_MOUSEWHEEL: case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
  case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_TEXTEDITING:
  case SDL_TEXTEDITING_EXT:
    return false;
  default:
    return true;
  }
}
