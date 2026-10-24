#pragma once
#include "AppTheme.h"
#include "RecyclerView.h"
#include "TextView.h"
#include "../library/ChartReadme.h"
#include "../library/DocumentLinks.h"
#include <optional>
#include <set>

namespace document_text {
struct Line { std::string text; std::size_t offset; };
}

// A virtualized document with URL hit regions spanning wrapped lines. Links are
// passive text: the recycler retains all scrolling and gesture ownership.
class DocumentTextReader : public RecyclerView<document_text::Line> {
public:
  std::function<void(const std::string &)> onOpenUrl;
  DocumentTextReader()
      : RecyclerView([](const auto &a, const auto &b) { return a.offset == b.offset; }),
        measure_(app_theme::font, 20) {
    measure_.setDeferredTextureMaterialization(true);
    reserveScrollbarGutter = true;
    isSelectable = [](const auto &, int) { return false; };
    onCreateView = [](const document_text::Line &) { return new Row(); };
    onBind = [this](View *view, const document_text::Line &item, int index, bool) {
      auto *row = static_cast<Row *>(view);
      row->text->setText(item.text);
      row->text->setHeight(itemHeight);
      row->markers->setHeight(itemHeight);
      row->markers->clearChildren();
      if (index < 0 || index >= static_cast<int>(regions_.size())) return;
      for (const auto &region : regions_[index]) {
        auto *underline = new View();
        underline->setName("documentLinkUnderline");
        underline->setPositionType(YGPositionTypeAbsolute);
        underline->setPosition(Edge::Left, region.left)->setPosition(Edge::Top, measure_.lineHeight());
        underline->setSize(std::max(1.0f, region.right - region.left), 1);
        underline->setThemedBackgroundColor(app_theme::cyan);
        row->markers->addView(underline);
      }
    };
  }
  void setDocumentText(std::string text, bool resetScroll = true) {
    if (text != text_) {
      cancelPointerInput();
      text_ = std::move(text);
      links_ = document_links::find(text_);
      width_ = -1;
    }
    if (resetScroll) { cancelPointerInput(); scrollOffset = 0; }
    reflow();
    rebindVisibleItems();
  }
  void onLayout() override {
    reflow();
    RecyclerView::onLayout();
  }
protected:
  bool handleEventsImpl(SDL_Event &event) override {
    if (sdl_pointer_event::isTouchSynthesizedMouse(event) ||
        sdl_pointer_event::isMouseSynthesizedTouch(event)) return RecyclerView::handleEventsImpl(event);
    float x = 0, y = 0;
    bool down = false, move = false, up = false, touch = false;
    SDL_FingerID id = 0;
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event.button.button != SDL_BUTTON_LEFT) break;
      rendering::screenToUi(event.button.x * rendering::widthScale,
                            event.button.y * rendering::heightScale, x, y);
      down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN; up = !down; break;
    case SDL_EVENT_MOUSE_MOTION:
      rendering::screenToUi(event.motion.x * rendering::widthScale,
                            event.motion.y * rendering::heightScale, x, y);
      move = true; break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
      rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, x, y);
      touch = true; id = event.tfinger.fingerID;
      down = event.type == SDL_EVENT_FINGER_DOWN;
      move = event.type == SDL_EVENT_FINGER_MOTION;
      up = event.type == SDL_EVENT_FINGER_UP; break;
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_FINGER_CANCELED:
      press_.reset(); break;
    default: break;
    }
    std::optional<std::string> activate;
    if (touch && down) {
      activeTouches_.insert(id);
      if (activeTouches_.size() > 1) multiTouch_ = true;
    }
    if (multiTouch_) press_.reset();
    if (down && !multiTouch_) {
      // A second pointer cancels activation rather than taking over the link.
      if (press_) press_.reset();
      else if (!isTouchScrolling()) {
        if (auto link = linkAt(x, y)) press_ = Press{*link, x, y, scrollOffset, touch, id};
      }
    } else if (press_ && press_->touch == touch && (!touch || press_->id == id)) {
      if (std::abs(x - press_->x) >= 12 || std::abs(y - press_->y) >= 12 ||
          std::abs(scrollOffset - press_->offset) > 0.5) {
        if (move || up) press_.reset();
      }
      if (up && press_) {
        if (linkAt(x, y) == press_->link) activate = links_[press_->link].url;
        press_.reset();
      }
    }
    if (touch && up) {
      activeTouches_.erase(id);
      if (activeTouches_.empty()) multiTouch_ = false;
    }
    const bool propagate = RecyclerView::handleEventsImpl(event);
    if (activate && onOpenUrl) {
      // A callback may rebuild its owner; do not touch the view afterwards.
      const auto open = onOpenUrl;
      open(*activate);
      return false;
    }
    return propagate;
  }
  void onPointerInputCancelled() override {
    press_.reset();
    activeTouches_.clear(); multiTouch_ = false;
    RecyclerView::onPointerInputCancelled();
  }
  void onPointerEventConsumed(const SDL_Event &event) override {
    press_.reset();
    if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
      activeTouches_.erase(event.tfinger.fingerID);
      if (activeTouches_.empty()) multiTouch_ = false;
    }
    RecyclerView::onPointerEventConsumed(event);
  }
private:
  class Row : public View {
  public:
    TextView *text = new TextView(app_theme::font, 20);
    View *markers = new View();
    Row() {
      text->setDeferredTextureMaterialization(true);
      text->setThemedColor(app_theme::textPrimary);
      text->setOverflow(TextView::TextOverflow::Hidden);
      for (View *child : {static_cast<View *>(text), markers}) {
        child->setPositionType(YGPositionTypeAbsolute);
        child->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0);
        child->setWidthPercent(100);
        addView(child);
      }
    }
  };
  class Measurer : public TextView {
  public:
    using TextView::TextView;
    int lineHeight() const { return textLineHeight(); }
  } measure_;
  struct Region { float left, right; std::size_t link; };
  struct Press { std::size_t link; float x, y; double offset; bool touch; SDL_FingerID id; };
  std::string text_;
  std::vector<document_links::Link> links_;
  std::vector<std::vector<Region>> regions_;
  std::optional<Press> press_;
  std::set<SDL_FingerID> activeTouches_;
  bool multiTouch_ = false;
  int width_ = -1;
  std::optional<std::size_t> linkAt(float x, float y) const {
    if (x < getContentX() || x >= getContentX() + getVisibleItemWidth() ||
        y < getContentY() || y >= getContentY() + getContentHeight()) return std::nullopt;
    const auto row = static_cast<std::size_t>((y - getContentY() + scrollOffset) / itemHeight);
    if (row >= regions_.size()) return std::nullopt;
    for (const auto &region : regions_[row])
      if (x - getContentX() >= region.left && x - getContentX() < region.right) return region.link;
    return std::nullopt;
  }
  void reflow() {
    const int width = std::max(1, getVisibleItemWidth() - 4);
    if (width == width_) return;
    width_ = width;
    if (width <= 1) return;
    press_.reset();
    const double offset = scrollOffset;
    itemHeight = std::max(30, measure_.lineHeight() + 4);
    auto lines = text_.empty() ? std::vector<std::string>{} : chart_readme::wrapLines(text_, width,
        [this](const auto &line) { return measure_.measureTextWidth(line); });
    regions_.clear(); regions_.resize(lines.size());
    std::vector<document_text::Line> items;
    items.reserve(lines.size());
    std::size_t start = 0, firstLink = 0;
    for (std::size_t row = 0; row < lines.size(); ++row) {
      const auto end = start + lines[row].size();
      while (firstLink < links_.size() && links_[firstLink].end <= start) ++firstLink;
      for (auto i = firstLink; i < links_.size() && links_[i].begin < end; ++i) {
        const auto begin = std::max(start, links_[i].begin) - start;
        const auto finish = std::min(end, links_[i].end) - start;
        regions_[row].push_back({static_cast<float>(measure_.measureTextWidth(lines[row].substr(0, begin))),
            static_cast<float>(measure_.measureTextWidth(lines[row].substr(0, finish))), i});
      }
      items.push_back({std::move(lines[row]), start});
      start = end;
      if (start < text_.size() && text_[start] == '\n') ++start;
    }
    setItems(std::move(items));
    scrollOffset = std::clamp(offset, 0.0,
        static_cast<double>(std::max(0, size() * itemHeight - getHeight())));
    rebindVisibleItems();
  }
};
