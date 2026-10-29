#pragma once
#include "FolderGoal.h"
#include "../i18n/Localization.h"
#include "../kombo/KomboModel.h"
namespace folder_goals {
inline std::string targetText(const Goal &g) {
  const auto value=percentText(g.target.percent/100.0);
  return g.target.type==Type::Lamp
      ? i18n::format("folder_goals.lamp_target",{{"percent",value},{"lamp",kombo::lampLabel(g.target.lamp)}})
      : i18n::format("folder_goals.ex_target",{{"percent",value}});
}
} // namespace folder_goals
