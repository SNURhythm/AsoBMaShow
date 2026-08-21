#pragma once

#include "LuaSkinFileSystem.h"
#include "../package/SkinPackageTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

struct Lr2SkinCommand {
  std::string name;
  std::vector<std::string> fields;
  SkinSourceLocation source;
  // The complete root-to-source stack, including the command's source file.
  std::vector<std::string> includeChain;
};

struct Lr2SkinParseResult {
  std::vector<Lr2SkinCommand> commands;
  std::vector<SkinDiagnostic> diagnostics;
  bool cancelled = false;
  bool fatal = false;
};

struct Lr2SkinCsvParserLimits {
  static constexpr std::size_t maxDocumentBytes = 8U * 1024U * 1024U;
  static constexpr std::size_t maxIncludeDepth = 32;

  std::size_t maximumDocumentBytes = maxDocumentBytes;
  std::size_t maximumIncludeDepth = maxIncludeDepth;
};

enum class Lr2IncludeExpansionMode : std::uint8_t {
  Eager,
  Preserve,
  ConditionAware,
};

struct Lr2SkinParseOptions {
  Lr2IncludeExpansionMode includeExpansion = Lr2IncludeExpansionMode::Eager;
  std::span<const int> enabledOptionIds;
};

class Lr2SkinCsvParser final {
public:
  explicit Lr2SkinCsvParser(Lr2SkinCsvParserLimits limits = {}) noexcept
      : limits_(limits) {}

  [[nodiscard]] Lr2SkinParseResult
  parse(LuaSkinFileSystem &, std::string_view entryPath,
        std::span<const std::byte> cp932Bytes,
        std::stop_token stop = {}, Lr2SkinParseOptions options = {}) const;

private:
  Lr2SkinCsvParserLimits limits_;
};

} // namespace skin
