#pragma once
#include "FolderGoal.h"
#include "FolderGoalStatusIndex.h"
#include "../i18n/Localization.h"
#include "../kombo/KomboModel.h"
namespace folder_goals {
inline std::string rateTargetText(const Target &target) {
  return target.type!=Type::Lamp && target.scoreRank>0
      ? kombo::scoreRankLabel(target.scoreRank) : percentText(target.type==Type::Lamp ? targetPercent(target) : scoreTargetPercent(target));
}
struct BubbleLabels { int lampRank=-1; std::string lamp, score, coverage; };
inline BubbleLabels bubbleLabels(const Status &status) {
  BubbleLabels labels;
  if (status.pendingLampCount>1 || status.pendingExCount>1 || status.pendingCoverageCount>1) {
    labels.score=i18n::format("goals.ui.count",{{"count",std::to_string(status.pendingLampCount+status.pendingExCount+status.pendingCoverageCount)}});
    return labels;
  }
  const auto progress=[](const PendingTarget &value) {
    return percentText(value.percent)+"/"+rateTargetText(value.target);
  };
  if (status.pendingLamp) {
    labels.lampRank=status.pendingLamp->target.lamp;
    labels.lamp=kombo::lampLabel(labels.lampRank)+" "+progress(*status.pendingLamp);
  }
  if (status.pendingEx) {
    const auto &value=*status.pendingEx;
    labels.score=i18n::format("folder_goals.ui.ex_bubble",{{"percent",progress(value)}});
  }
  if (status.pendingCoverage) {
    const auto &value=*status.pendingCoverage;
    labels.coverage=i18n::format("folder_goals.ui.ex_coverage_bubble",{{"score",rateTargetText(value.target)},
        {"percent",percentText(value.percent)+"/"+percentText(targetPercent(value.target))}});
  }
  return labels;
}
inline std::string targetText(const Goal &g) {
  if (g.target.type==Type::ExCoverage)
    return i18n::format("folder_goals.ui.ex_coverage_target",{{"percent",percentText(targetPercent(g.target))},
        {"score",rateTargetText(g.target)}});
  const auto value=rateTargetText(g.target);
  return g.target.type==Type::Lamp
      ? i18n::format("folder_goals.ui.lamp_target",{{"percent",value},{"lamp",kombo::lampLabel(g.target.lamp)}})
      : i18n::format("folder_goals.ui.ex_target",{{"percent",value}});
}
} // namespace folder_goals
