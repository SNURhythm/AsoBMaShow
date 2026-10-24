#pragma once
#include "AppTheme.h"
#include "RecyclerView.h"
#include "TextView.h"
#include "../library/ChartReadme.h"

// Recycle visible lines instead of creating one potentially enormous text texture.
class LegalNoticeReader : public RecyclerView<std::string> {
public:
  LegalNoticeReader()
      : RecyclerView([](const auto &a, const auto &b) { return a == b; }),
        measure_(app_theme::font, 20) {
    measure_.setDeferredTextureMaterialization(true);
    setName("legalNoticeReader");
    reserveScrollbarGutter = true;
    isSelectable = [](const auto &, int) { return false; };
    onCreateView = [](const std::string &) {
      auto *line = new TextView(app_theme::font, 20);
      line->setDeferredTextureMaterialization(true);
      line->setThemedColor(app_theme::textPrimary);
      line->setOverflow(TextView::TextOverflow::Hidden);
      return line;
    };
    onBind = [](View *view, const std::string &line, int, bool) {
      static_cast<TextView *>(view)->setText(line);
    };
  }
  void setNotice(std::string text) {
    text_ = std::move(text);
    width_ = -1;
    scrollOffset = 0;
    reflow();
  }
  void onLayout() override {
    reflow();
    RecyclerView::onLayout();
  }
private:
  class Measurer : public TextView {
  public:
    using TextView::TextView;
    int lineHeight() const { return textLineHeight(); }
  } measure_;
  std::string text_;
  int width_ = -1;
  void reflow() {
    const int width = std::max(1, getVisibleItemWidth() - 4);
    if (width == width_) return;
    width_ = width;
    // Before Yoga assigns a width, avoid wrapping the entire license at 1px.
    if (width <= 1) return;
    const double offset = scrollOffset;
    itemHeight = std::max(30, measure_.lineHeight() + 4);
    setItems(chart_readme::wrapLines(text_, width,
        [this](const auto &line) { return measure_.measureTextWidth(line); }));
    scrollOffset = std::clamp(offset, 0.0,
        static_cast<double>(std::max(0, size() * itemHeight - getHeight())));
    rebindVisibleItems();
  }
};
