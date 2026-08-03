#pragma once

#include "../SkinPresentationTypes.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace skin {

struct SkinPackagePolicy {
  static constexpr std::uint64_t maxArchiveBytes = 2ULL * 1024 * 1024 * 1024;
  static constexpr std::uint64_t maxRegularFileBytes = 512ULL * 1024 * 1024;
  static constexpr std::uint64_t maxExpandedBytes = 4ULL * 1024 * 1024 * 1024;
  static constexpr std::uint64_t maxFiles = 20'000;
  static constexpr std::uint64_t maxArchiveMembers = 65'535;
  static constexpr std::uint32_t maxPathBytes = 1'024;
  static constexpr std::uint32_t maxPathComponents = 64;
  static constexpr std::uint32_t maxPackageNameBytes = 128;
};

enum class PackageCollisionPolicy : std::uint8_t {
  Reject,
  Replace,
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

enum class SkinValidationDisposition : std::uint8_t {
  Selectable7Key,
  UnavailableType,
  Invalid,
};

struct SkinCatalogCategoryDeclaration {
  std::string name;
  std::vector<std::string> items;
};

struct SkinCatalogOptionChoice {
  std::string label;
  int value = 0;
};

struct SkinCatalogOptionDeclaration {
  std::string category;
  std::string name;
  std::vector<SkinCatalogOptionChoice> choices;
  std::string defaultLabel;
};

struct SkinCatalogFileDeclaration {
  std::string category;
  std::string name;
  std::string pattern;
  std::string defaultValue;
  std::vector<std::string> choices;
};

struct SkinCatalogOffsetDeclaration {
  std::string category;
  std::string name;
  int id = 0;
  std::uint8_t permissions = 0;
};

struct SkinEntryMetadataSnapshot {
  std::string displayName;
  std::string author;
  int skinType = -1;
  int authoredWidth = 0;
  int authoredHeight = 0;
  std::vector<SkinCatalogCategoryDeclaration> categories;
  std::vector<SkinCatalogOptionDeclaration> options;
  std::vector<SkinCatalogFileDeclaration> files;
  std::vector<SkinCatalogOffsetDeclaration> offsets;
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
