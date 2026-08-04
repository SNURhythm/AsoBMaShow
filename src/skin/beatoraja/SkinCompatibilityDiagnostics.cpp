#include "SkinCompatibilityDiagnostics.h"

#include <algorithm>
#include <utility>

namespace skin {
namespace {

bool sameSource(const std::optional<SkinSourceLocation> &left,
                const std::optional<SkinSourceLocation> &right) {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  return !left || (left->virtualPath == right->virtualPath &&
                   left->line == right->line && left->column == right->column);
}

bool sameOccurrence(const SkinCompatibilityDiagnostic &existing,
                    const SkinDiagnostic &incoming, std::string_view objectId) {
  return existing.objectId == objectId &&
         existing.diagnostic.code == incoming.code &&
         existing.diagnostic.virtualPath == incoming.virtualPath &&
         existing.diagnostic.severity == incoming.severity &&
         sameSource(existing.diagnostic.source, incoming.source);
}

} // namespace

void SkinCompatibilityDiagnostics::report(SkinDiagnostic diagnostic,
                                          bool critical,
                                          std::string_view objectId) {
  const auto existing = std::ranges::find_if(
      entries_, [&](const SkinCompatibilityDiagnostic &entry) {
        return sameOccurrence(entry, diagnostic, objectId);
      });
  if (existing != entries_.end()) {
    existing->critical = existing->critical || critical;
    hasCritical_ = hasCritical_ || critical;
    return;
  }
  entries_.push_back({.diagnostic = std::move(diagnostic),
                      .objectId = std::string(objectId),
                      .critical = critical});
  hasCritical_ = hasCritical_ || critical;
}

std::span<const SkinCompatibilityDiagnostic>
SkinCompatibilityDiagnostics::entries() const noexcept {
  return entries_;
}

bool SkinCompatibilityDiagnostics::hasCritical() const noexcept {
  return hasCritical_;
}

void SkinCompatibilityDiagnostics::clear() noexcept {
  entries_.clear();
  hasCritical_ = false;
}

} // namespace skin
