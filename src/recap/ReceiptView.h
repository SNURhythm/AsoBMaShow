#pragma once
#include "LampBadge.h"
#include "../view/View.h"
#include "../view/TextView.h"
#include "../view/AppTheme.h"
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace recap {
struct ReceiptEntry {
  std::string title, mode, before, after, delta, group;
  std::string difficulty = "Lv. —";
  std::optional<int> beforeLamp, afterLamp;
};
struct ReceiptSection { std::string title, empty; std::vector<ReceiptEntry> entries; };
struct ReceiptContent {
  std::string title, profile, period, footer;
  std::array<ReceiptSection,3> sections;
};
inline constexpr int receiptWidth = 960;
namespace receipt_detail {
inline const Color paper{250,247,239}, ink{36,43,43}, muted{107,112,108}, accent{24,112,98}, rule{216,212,201};
class Text final : public TextView {
public:
  Text(int size, bool bold) : TextView(app_theme::font,size,bold ? FontWeight::Bold : FontWeight::Regular) {
    setDeferredTextureMaterialization(true);
  }
protected:
  void renderImpl(RenderContext &context) override {
    TextView::renderImpl(context);
    // Keep raster memory bounded across tall exports. bgfx retires the texture
    // after submitted draws; the next strip can rasterize a straddling line again.
    context.flushUiBatch();
    invalidateTexture();
  }
};
inline Text *text(View *parent,const std::string &value,int size,Color color=ink,bool bold=false) {
  auto *label=new Text(size,bold);
  label->setText(value); label->setWrap(true); label->setColor({color.r,color.g,color.b,255});
  label->setFlexShrink(0)->setMinHeight(size+4);
  parent->addView(label); return label;
}
inline View *column(View *parent,int gap=12) {
  auto *view=new View(); view->setFlexDirection(FlexDirection::Column)->setGap(gap)->setFlexShrink(0);
  parent->addView(view); return view;
}
inline View *row(View *parent,int gap=16) {
  auto *view=new View(); view->setFlexDirection(FlexDirection::Row)->setGap(gap)->setFlexShrink(0);
  parent->addView(view); return view;
}
}
inline std::unique_ptr<View> makeReceipt(const ReceiptContent &content) {
  using namespace receipt_detail;
  View::LayoutBatchScope batch;
  auto root=std::make_unique<View>();
  root->setWidth(receiptWidth)->setFlexDirection(FlexDirection::Column)
      ->setGap(4)->setPadding(Edge::All,28)->setBackgroundColor(paper);
  auto *brand=row(root.get(),12);
  text(brand,"AsoBMaShow",20,accent,true)->setFlex(1)->setMinWidth(0);
  auto *profile=text(brand,content.profile,18,muted);
  profile->setAlign(TextView::RIGHT); profile->setFlex(1)->setMinWidth(0);
  text(root.get(),content.title,30,ink,true)->setAlign(TextView::CENTER);
  text(root.get(),content.period,18,muted)->setAlign(TextView::CENTER);
  for (const auto &section:content.sections) {
    auto *heading=row(root.get());
    heading->setMargin(Edge::Top,14)->setPadding(Edge::All,6)->setCornerRadius(4)->setBackgroundColor(rule);
    text(heading,section.title,20,ink,true)->setFlex(1)->setMinWidth(0);
    text(heading,std::to_string(section.entries.size()),20,ink,true);
    if (section.entries.empty()) text(root.get(),section.empty,18,muted);
    std::string previousGroup;
    for (const auto &entry:section.entries) {
      if (!entry.group.empty() && entry.group != previousGroup) {
        auto *heading=text(root.get(),entry.group,18,accent,true);
        heading->setMargin(Edge::Top,6);
        if (entry.afterLamp) styleLampBadge(heading,*entry.afterLamp,true);
        heading->setAlignSelf(YGAlignStretch);
        previousGroup=entry.group;
      }
      auto *item=row(root.get(),10);
      item->setName("recapEntry");
      item->setAlignItems(YGAlignCenter)->setPadding(Edge::All,6)
          ->setCornerRadius(3)->setBackgroundColor({255,255,255});
      auto *difficulty=column(item,0);
      difficulty->setWidth(108);
      text(difficulty,entry.difficulty,18,ink,true);
      text(difficulty,entry.mode,14,muted);
      auto *values=row(item,5);
      values->setWidth(250);
      values->setFlexWrap(YGWrapWrap)->setAlignItems(YGAlignCenter);
      auto *before=text(values,entry.before,18,muted);
      before->setFlexShrink(1)->setMinWidth(0);
      if (entry.beforeLamp) styleLampBadge(before,*entry.beforeLamp,true);
      else if (!entry.afterLamp) { before->setWidth(70); before->setAlign(TextView::RIGHT); }
      text(values,"→",18,muted);
      auto *after=text(values,entry.after,18,accent,true);
      after->setFlexShrink(1)->setMinWidth(0);
      if (entry.afterLamp) styleLampBadge(after,*entry.afterLamp,true);
      else after->setWidth(70);
      if (!entry.delta.empty()) text(values,entry.delta,16,accent)->setFlexShrink(1)->setMinWidth(0);
      text(item,entry.title,20,ink)->setFlex(1)->setMinWidth(0);
    }
  }
  auto *footer=text(root.get(),content.footer,16,muted);
  footer->setMargin(Edge::Top,14);
  footer->setAlign(TextView::CENTER);
  return root;
}
} // namespace recap
