#pragma once
#include "../view/View.h"
#include "../view/TextView.h"
#include "../view/AppTheme.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace recap {
struct ReceiptEntry { std::string title, mode, before, after, delta, group; };
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
  label->setFlexShrink(0)->setMinHeight(size+8);
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
inline void divider(View *parent) {
  auto *line=row(parent,8);
  line->setMargin(Edge::Top,12)->setMargin(Edge::Bottom,12);
  for (int i=0;i<42;++i) {
    auto *dash=new View(); dash->setWidth(12)->setHeight(2)->setBackgroundColor(rule); line->addView(dash);
  }
}
}
inline std::unique_ptr<View> makeReceipt(const ReceiptContent &content) {
  using namespace receipt_detail;
  View::LayoutBatchScope batch;
  auto root=std::make_unique<View>();
  root->setWidth(receiptWidth)->setMinHeight(720)->setFlexDirection(FlexDirection::Column)
      ->setGap(20)->setPadding(Edge::All,64)->setBackgroundColor(paper);
  text(root.get(),"A S O B M A S H O W",20,accent,true);
  text(root.get(),content.title,48,ink,true);
  text(root.get(),content.profile,26,ink,true);
  text(root.get(),content.period,20,muted);
  divider(root.get());
  auto *summary=row(root.get(),24);
  for (const auto &section:content.sections) {
    auto *item=column(summary,4); item->setFlex(1)->setMinWidth(0);
    text(item,std::to_string(section.entries.size()),48,accent,true);
    text(item,section.title,20,muted);
  }
  for (const auto &section:content.sections) {
    divider(root.get());
    auto *heading=row(root.get());
    text(heading,section.title,28,ink,true)->setFlex(1)->setMinWidth(0);
    text(heading,std::to_string(section.entries.size()),28,accent,true);
    if (section.entries.empty()) text(root.get(),section.empty,22,muted);
    std::string previousGroup;
    for (const auto &entry:section.entries) {
      if (!entry.group.empty() && entry.group != previousGroup) {
        text(root.get(),entry.group,22,accent,true)->setMargin(Edge::Top,12);
        previousGroup=entry.group;
      }
      auto *item=column(root.get(),10);
      item->setPadding(Edge::Top,8)->setPadding(Edge::Bottom,12);
      text(item,entry.title,28,ink,true);
      text(item,entry.mode,18,muted);
      auto *values=row(item,12);
      text(values,entry.before+"  →",22,muted)->setFlexShrink(1)->setMinWidth(0);
      text(values,entry.after,24,accent,true)->setFlexShrink(1)->setMinWidth(0);
      if (!entry.delta.empty()) text(values,entry.delta,20,accent)->setFlexShrink(1)->setMinWidth(0);
      auto *line=new View(); line->setHeight(1)->setMargin(Edge::Top,10)->setBackgroundColor(rule); item->addView(line);
    }
  }
  divider(root.get());
  auto *footer=text(root.get(),content.footer,20,muted);
  footer->setAlign(TextView::CENTER);
  return root;
}
} // namespace recap
