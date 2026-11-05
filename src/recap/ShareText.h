#pragma once
#include "PlayRecap.h"

namespace recap {
inline std::string shareText(const Snapshot &snapshot, const std::string &title,
                            const std::string &period, const std::string &filters,
                            const std::array<std::string,3> &sectionNames) {
  std::string text = title + "\n" + period + "\n";
  for (std::size_t kind = 0; kind < snapshot.sections.size(); ++kind) {
    if (kind) text += " · ";
    text += sectionNames[kind] + ": " + std::to_string(snapshot.sections[kind].size());
  }
  if (!filters.empty()) text += "\n" + filters;
  return text + "\n#AsoBMaShow";
}
} // namespace recap
