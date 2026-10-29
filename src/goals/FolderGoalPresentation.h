#pragma once
#include "FolderGoal.h"
#include "FolderGoalStatusIndex.h"
#include "../i18n/Localization.h"
#include "../kombo/KomboModel.h"
namespace folder_goals {
struct BubbleLabels { int lampRank=-1; std::string lamp, score; };
inline BubbleLabels bubbleLabels(const Status &status) {
  BubbleLabels labels;
  if (status.pendingLampCount>1 || status.pendingExCount>1) {
    labels.score=i18n::format("goals.ui.count",{{"count",std::to_string(status.pendingLampCount+status.pendingExCount)}});
    return labels;
  }
  const auto progress=[](const PendingTarget &value) {
    return percentText(value.percent)+"/"+percentText(value.target.percent/100.0);
  };
  if (status.pendingLamp) {
    labels.lampRank=status.pendingLamp->target.lamp;
    labels.lamp=kombo::lampLabel(labels.lampRank)+" "+progress(*status.pendingLamp);
  }
  if (status.pendingEx) labels.score=i18n::format("folder_goals.ui.ex_bubble",
      {{"percent",progress(*status.pendingEx)}});
  return labels;
}
inline std::string targetText(const Goal &g) {
  const auto value=percentText(g.target.percent/100.0);
  return g.target.type==Type::Lamp
      ? i18n::format("folder_goals.ui.lamp_target",{{"percent",value},{"lamp",kombo::lampLabel(g.target.lamp)}})
      : i18n::format("folder_goals.ui.ex_target",{{"percent",value}});
}
} // namespace folder_goals
