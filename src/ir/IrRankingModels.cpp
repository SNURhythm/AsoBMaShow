#include "IrRankingModels.h"

#include "IrOutboxModels.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>

namespace ir {
namespace {

std::string normalizedHash(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

bool isHexDigest(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

IrChartQueryBuildOutcome invalid(std::string_view diagnostic) {
  return {.diagnostic = sanitizeDiagnostic(diagnostic)};
}

} // namespace

IrChartQueryBuildOutcome
makeIrChartQuery(const bms_parser::ChartMeta &meta) noexcept {
  try {
    if (meta.KeyMode <= 0 || meta.TotalNotes <= 0 ||
        meta.TotalNotes > std::numeric_limits<int>::max() / 2) {
      return invalid("chart key mode or note count is invalid");
    }
    const std::string md5 = normalizedHash(meta.MD5);
    const std::string sha256 = normalizedHash(meta.SHA256);
    if ((!md5.empty() && !isHexDigest(md5, 32)) ||
        (!sha256.empty() && !isHexDigest(sha256, 64)) ||
        (md5.empty() && sha256.empty())) {
      return invalid("chart hash identity is malformed");
    }
    return {.value = IrChartQuery{.keyMode = meta.KeyMode,
                                  .chartMd5 = md5,
                                  .chartSha256 = sha256,
                                  .totalNotes = meta.TotalNotes}};
  } catch (...) {
    return invalid("chart ranking query construction failed");
  }
}

} // namespace ir
