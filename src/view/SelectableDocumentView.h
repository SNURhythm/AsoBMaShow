#pragma once

#include "DocumentTextReader.h"
#include "../platform/NativeDocument.h"

// Owns one OS text surface with a virtualized fallback for headless/no-window
// contexts. The native surface has no scene callback and cannot outlive this view.
class SelectableDocumentView : public View {
public:
  using Factory = std::function<std::unique_ptr<native_document::Surface>(
      const std::string &, native_document::Frame, native_document::Appearance)>;
  explicit SelectableDocumentView(Factory factory = native_document::create);
  void open(std::string text);
  void close();
  void highlight(const std::vector<text_range::Range> &ranges, bool focus);
  void scroll(native_document::Scroll action);
  bool hasNativeSurface() const { return native_ != nullptr; }
  bool dismissRequested() const { return native_ && native_->dismissRequested(); }
  void onLayout() override;
  void propagateThemeChange() override;
private:
  Factory factory_;
  std::unique_ptr<native_document::Surface> native_;
  DocumentTextReader *fallback_ = nullptr;
  native_document::Frame nativeFrame() const;
  native_document::Appearance nativeAppearance() const;
};
