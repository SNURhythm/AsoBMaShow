#include "SelectableDocumentView.h"

SelectableDocumentView::SelectableDocumentView(Factory factory) : factory_(std::move(factory)) {
  setFlexDirection(FlexDirection::Column);
  fallback_ = new DocumentTextReader();
  fallback_->setName("chartSourceText");
  fallback_->setFlex(1)->setMinHeight(0)->setWidthPercent(100);
  addView(fallback_);
}
void SelectableDocumentView::open(std::string text) {
  close();
  applyYogaLayout();
  native_ = factory_(text, nativeFrame(), nativeAppearance());
  fallback_->setVisible(!native_);
  if (native_) native_->setSyntax(bms_syntax::tokenize(text));
  else fallback_->setDocumentText(std::move(text));
}
void SelectableDocumentView::close() {
  native_.reset();
  fallback_->setDocumentText("");
  fallback_->setVisible(true);
}
void SelectableDocumentView::highlight(const std::vector<text_range::Range> &ranges, bool focus) {
  if (native_) native_->highlight(ranges, focus);
  else fallback_->setHighlights(ranges, focus);
}
void SelectableDocumentView::scroll(native_document::Scroll action) {
  if (native_) { native_->scroll(action); return; }
  using S = native_document::Scroll;
  double offset = fallback_->scrollOffset;
  switch (action) {
  case S::LineUp: offset -= fallback_->itemHeight; break;
  case S::LineDown: offset += fallback_->itemHeight; break;
  case S::PageUp: offset -= fallback_->getContentHeight(); break;
  case S::PageDown: offset += fallback_->getContentHeight(); break;
  case S::Start: offset = 0; break;
  case S::End: offset = fallback_->size() * fallback_->itemHeight; break;
  }
  fallback_->scrollOffset = std::clamp(offset, 0.0,
      static_cast<double>(std::max(0, fallback_->size() * fallback_->itemHeight - fallback_->getContentHeight())));
  fallback_->rebindVisibleItems();
}
native_document::Frame SelectableDocumentView::nativeFrame() const {
  const float width = std::max(1, rendering::render_width);
  const float height = std::max(1, rendering::render_height);
  return {(rendering::ui_offset_x + getContentX() * rendering::ui_scale_x) / width,
          (rendering::ui_offset_y + getContentY() * rendering::ui_scale_y) / height,
          getContentWidth() * rendering::ui_scale_x / width,
          getContentHeight() * rendering::ui_scale_y / height};
}
native_document::Appearance SelectableDocumentView::nativeAppearance() const {
  const auto rgba = [](Color color) {
    const auto value = app_theme::sdl(color);
    return (std::uint32_t(value.r) << 24) | (std::uint32_t(value.g) << 16) |
           (std::uint32_t(value.b) << 8) | value.a;
  };
  native_document::Appearance appearance{rgba(app_theme::textPrimary()), rgba(app_theme::panelStrong()),
      rgba(app_theme::withAlpha(app_theme::amber(), 90)), 16};
  const bool light = app_theme::activeMode() == app_theme::ThemeMode::Light;
  appearance.syntax = {rgba(app_theme::cyan()), rgba(light ? Color(112, 61, 165, 255) : Color(198, 154, 245, 255)),
      rgba(app_theme::amber()), rgba(app_theme::coral()), rgba(app_theme::lime()),
      rgba(app_theme::textSecondary()), rgba(light ? Color(50, 84, 128, 255) : Color(178, 198, 229, 255)),
      rgba(app_theme::textMuted())};
  return appearance;
}
void SelectableDocumentView::onLayout() {
  if (native_) native_->setFrame(nativeFrame());
}
void SelectableDocumentView::propagateThemeChange() {
  View::propagateThemeChange();
  if (native_) native_->setAppearance(nativeAppearance());
}
