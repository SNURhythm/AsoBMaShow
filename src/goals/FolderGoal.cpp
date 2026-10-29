#include "FolderGoal.h"
#include "../ClearLampOrder.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace folder_goals {
double scoreTargetPercent(const Target &t) {
  return t.scoreRank>0 ? 100.0*t.scoreRank/27
      : (t.type==Type::ExCoverage ? t.exPercent : t.percent)/100.0;
}
double targetPercent(const Target &t) {
  return t.type==Type::AverageEx ? scoreTargetPercent(t) : t.percent/100.0;
}
void validate(const Target &t) {
  if ((t.type != Type::Lamp && t.type != Type::AverageEx && t.type != Type::ExCoverage) || t.percent < 1 || t.percent > 10000)
    throw std::invalid_argument("Choose a percentage above 0 and at most 100, with up to two decimal places.");
  if (t.scoreRank!=0 && (t.type==Type::Lamp ||
      std::find(scoreRanks.begin(),scoreRanks.end(),t.scoreRank)==scoreRanks.end()))
    throw std::invalid_argument("Choose a valid average EX score rank.");
  if (t.exPercent<1 || t.exPercent>10000)
    throw std::invalid_argument("Choose a valid minimum EX rate.");
  if (t.type == Type::Lamp && t.lamp != 0 && t.lamp != 50 && t.lamp != 100 &&
      t.lamp != 200 && t.lamp != 300 && t.lamp != 400 && t.lamp != 500 && t.lamp != 600)
    throw std::invalid_argument("Choose a valid minimum lamp.");
}
void validate(const Goal &g) {
  validate(g.target);
  if (g.source.kind < SourceKind::Directory || g.source.kind > SourceKind::Level ||
      g.source.identity.empty() || g.source.identity.size() > 16384 ||
      g.source.name.size() > 16384 || g.source.level.size() > 4096 ||
      g.source.identity.find('\0') != std::string::npos ||
      g.id.size() > 32768 || g.id.find('\0') != std::string::npos ||
      (g.source.kind != SourceKind::Level && !g.source.level.empty()) ||
      g.longNoteMode < 1 || g.longNoteMode > 3 || g.revision < 1 ||
      g.setAt < 0 || g.achievedAt < 0 || g.setAt > 253402300799999LL ||
      g.achievedAt > 253402300799999LL || (g.achievedAt && g.achievedAt < g.setAt))
    throw std::invalid_argument("Invalid folder goal.");
}
std::string sourceKey(const Source &s) {
  return std::to_string(static_cast<int>(s.kind)) + ":" + std::to_string(s.identity.size()) +
      ":" + s.identity + ":" + s.level;
}
std::string scopeKey(const Goal &g) {
  return sourceKey(g.source) + ":" + std::to_string(g.longNoteMode) + ":" +
      std::to_string(static_cast<int>(g.target.type));
}
std::string key(const Goal &g) { return g.id; }
Progress evaluate(const Target &t, std::span<const Member> members) {
  validate(t);
  Progress p;
  std::unordered_map<std::string, const Member *> unique;
  for (const auto &m : members) {
    auto [it, inserted] = unique.emplace(m.identity, &m);
    if (!inserted && !it->second->available && m.available) it->second = &m;
  }
  long double sum = 0, compensation = 0;
  for (const auto &[id, row] : unique) {
    const auto &m = *row;
    if (!m.available) { ++p.missing; continue; }
    ++p.available;
    if (t.type==Type::Lamp && clearLampOrder(m.lamp) >= clearLampOrder(t.lamp)) ++p.qualifying;
    if (m.maxScore <= 0) { ++p.unknownScore; continue; }
    if (t.type==Type::ExCoverage) {
      const int numerator=t.scoreRank>0 ? t.scoreRank : t.exPercent;
      const int denominator=t.scoreRank>0 ? 27 : 10000;
      if (static_cast<std::int64_t>(m.score)*denominator>=static_cast<std::int64_t>(m.maxScore)*numerator)
        ++p.qualifying;
    }
    const long double value = std::clamp(static_cast<long double>(m.score) / m.maxScore, 0.L, 1.L);
    const long double adjusted = value - compensation;
    const auto next = sum + adjusted;
    compensation = (next - sum) - adjusted;
    sum = next;
  }
  p.required = (p.available * t.percent + 9999) / 10000;
  if (!p.available) return p;
  if (t.type != Type::AverageEx) {
    p.percent = 100.0 * p.qualifying / p.available;
    p.achieved = p.qualifying >= p.required;
  } else {
    p.percent = static_cast<double>(sum * 100 / p.available);
    // Compensated long-double sum; tolerate only floating-point roundoff at an
    // exact threshold, many orders below the smallest supported target step.
    const long double threshold = t.scoreRank>0
        ? static_cast<long double>(t.scoreRank) * p.available / 27
        : static_cast<long double>(t.percent) * p.available / 10000;
    p.achieved = sum >= threshold || std::abs(sum-threshold) <=
        8 * std::numeric_limits<long double>::epsilon() * p.available;
  }
  return p;
}
std::string percentText(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << value << '%';
  return out.str();
}
} // namespace folder_goals
