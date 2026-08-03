#pragma once

#include "../SkinPresentationTypes.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace skin {

struct SkinPackagePolicy {
  static constexpr std::uint64_t maxArchiveBytes = 2ULL * 1024 * 1024 * 1024;
  static constexpr std::uint64_t maxRegularFileBytes =
      512ULL * 1024 * 1024;
  static constexpr std::uint64_t maxExpandedBytes =
      4ULL * 1024 * 1024 * 1024;
  static constexpr std::uint64_t maxFiles = 20'000;
  static constexpr std::uint32_t maxPathBytes = 1'024;
  static constexpr std::uint32_t maxPathComponents = 64;
  static constexpr std::uint32_t maxPackageNameBytes = 128;
};

struct SkinPackageId {
  std::string directoryName;
  std::string collisionKey;

  auto operator<=>(const SkinPackageId &) const = default;
};

struct SkinEntryId {
  SkinPackageId package;
  std::string packageRelativePath;
  std::string collisionKey;

  auto operator<=>(const SkinEntryId &) const = default;
};

enum class DiagnosticSeverity : std::uint8_t {
  Info,
  Warning,
  Error,
};

enum class SkinProgressPhase : std::uint8_t {
  Inspecting,
  Copying,
  Validating,
  Publishing,
};

struct SkinSourceLocation {
  std::string virtualPath;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

struct SkinDiagnostic {
  std::string code;
  std::string message;
  std::string virtualPath;
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  std::optional<SkinSourceLocation> source;
};

struct SkinProgress {
  SkinProgressPhase phase = SkinProgressPhase::Inspecting;
  std::uint64_t completedBytes = 0;
  std::uint64_t totalBytes = 0;
  std::uint64_t completedFiles = 0;
};

using SkinProgressCallback = std::function<void(const SkinProgress &)>;

struct SkinRevision {
  SkinPackageId package;
  std::string lowercaseSha256;
  std::uint64_t fileCount = 0;
  std::uint64_t totalBytes = 0;
};

struct SkinPackageIdResult {
  std::optional<SkinPackageId> package;
  std::string error;
};

struct SkinEntryIdResult {
  std::optional<SkinEntryId> entry;
  std::string error;
};

struct SkinUtf8NfcResult {
  std::optional<std::string> value;
  std::string error;
};

} // namespace skin
