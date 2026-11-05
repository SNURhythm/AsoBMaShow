#pragma once
#include "../view/AppTheme.h"
#include "../view/IconText.h"
#include "../view/ImageView.h"
#include "../view/TextView.h"

namespace recap {
class JacketView final : public View {
public:
  explicit JacketView(const std::filesystem::path &path, bool exporting = false)
      : image_(0,0,size,size), placeholder_(ui_icons::kFontAwesomeSolidPath,22),
        path_(fspath_to_path_t(path)), exporting_(exporting) {
    setName("recapJacket");
    setWidth(size)->setHeight(size)->setFlexShrink(0)->setCornerRadius(3);
    if (exporting) setBackgroundColor({235,234,228});
    else setThemedBackgroundColor(app_theme::insetSurface);
    image_.setContentMode(ImageView::ContentMode::Contain);
    placeholder_.setDeferredTextureMaterialization(true);
    placeholder_.setText(ui_icons::textForCodepoint(ui_icons::kMusic));
    placeholder_.setWidth(size)->setHeight(size);
    placeholder_.setAlign(TextView::CENTER); placeholder_.setVAlign(TextView::MIDDLE);
    placeholder_.applyYogaLayout();
    if (!exporting && !path_.empty()) image_.setImageAsync(path_);
  }
protected:
  void renderImpl(RenderContext &context) override {
    // Exports visit only visible strips. Load the thumbnail synchronously here,
    // then retire its GPU texture after drawing instead of loading every jacket
    // or racing async thumbnail completion against the single export frame.
    if (exporting_ && !path_.empty() && !failed_) failed_ = !image_.setImage(path_);
    image_.setPositionNoLayout(getX(),getY());
    image_.render(context);
    if (image_.imageWidth() <= 0 || image_.imageHeight() <= 0) {
      const auto color = exporting_ ? Color(107,112,108) : app_theme::textSecondary();
      placeholder_.setColor({color.r,color.g,color.b,255});
      placeholder_.setPositionNoLayout(getX(),getY());
      placeholder_.render(context);
    }
    if (exporting_) {
      context.flushUiBatch();
      image_.freeImage();
      placeholder_.releaseTexture();
    }
  }
private:
  class Placeholder final : public TextView {
  public:
    using TextView::TextView;
    void releaseTexture() { invalidateTexture(); }
  };
  static constexpr int size = 40;
  ImageView image_;
  Placeholder placeholder_;
  path_t path_;
  bool exporting_ = false, failed_ = false;
};
} // namespace recap
