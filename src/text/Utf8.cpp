#include "Utf8.h"

// Keep the restricted amalgamation private and compile it exactly once, also
// in Xcode's synchronized src group. The .inc is not a separate source file.
#include "simdutf/simdutf.cpp.inc"

namespace asobmashow::text {

bool validUtf8(std::string_view value) noexcept {
  return value.empty() || simdutf::validate_utf8(value.data(), value.size());
}

bool decodeUtf8(std::string_view value, std::vector<char32_t> &output) {
  if (value.empty()) {
    output.clear();
    return true;
  }
  // One scalar per byte is the upper bound. resize (not reserve) gives the
  // converter live char32_t objects to write, including on the malformed path.
  output.resize(value.size());
  const auto result = simdutf::convert_utf8_to_utf32_with_errors(
      value.data(), value.size(), output.data());
  if (result.error != simdutf::SUCCESS) {
    output.clear();
    return false;
  }
  output.resize(result.count);
  return true;
}

} // namespace asobmashow::text
