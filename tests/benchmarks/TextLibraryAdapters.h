#pragma once

// Experiment only: this header is injected into a temporary source snapshot.
// Includes the production wrapper for comparison with the library adapters.
#include <cstddef>
#include <iterator>
#include <string_view>
#include <vector>
#include <utf8proc.h>

#if TEXT_LIBRARY_BACKEND == 2
#include <simdutf.h>
#elif TEXT_LIBRARY_BACKEND == 3
#include <utf8.h>
#elif TEXT_LIBRARY_BACKEND == 4
#include "text/Utf8.h"
#endif

namespace text_library_experiment {

#if TEXT_LIBRARY_BACKEND == 1
inline constexpr std::string_view name = "utf8proc";
#elif TEXT_LIBRARY_BACKEND == 2
inline constexpr std::string_view name = "simdutf";
#elif TEXT_LIBRARY_BACKEND == 3
inline constexpr std::string_view name = "utfcpp";
#elif TEXT_LIBRARY_BACKEND == 4
inline constexpr std::string_view name = "production";
#else
#error Select TEXT_LIBRARY_BACKEND=1 (utf8proc), 2 (simdutf), 3 (utfcpp), or 4 (production)
#endif

inline bool decode(std::string_view value, std::vector<char32_t> &output) {
#if TEXT_LIBRARY_BACKEND == 4
  return asobmashow::text::decodeUtf8(value, output);
#endif
  output.clear();
  if (value.empty()) return true;
#if TEXT_LIBRARY_BACKEND == 2
  // At most one UTF-32 unit per input byte, including on malformed input.
  // Resize creates writable objects; writing beyond vector::size is invalid.
  output.resize(value.size());
  const auto result = simdutf::convert_utf8_to_utf32_with_errors(
      value.data(), value.size(), output.data());
  if (result.error != simdutf::SUCCESS) {
    output.clear();
    return false;
  }
  output.resize(result.count);
#elif TEXT_LIBRARY_BACKEND == 3
  output.reserve(value.size());
  try {
    utf8::utf8to32(value.begin(), value.end(), std::back_inserter(output));
  } catch (const utf8::exception &) {
    output.clear();
    return false;
  }
#else
  output.reserve(value.size());
  for (std::size_t offset = 0; offset < value.size();) {
    utf8proc_int32_t codepoint = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t *>(value.data() + offset),
        static_cast<utf8proc_ssize_t>(value.size() - offset), &codepoint);
    if (consumed <= 0) {
      output.clear();
      return false;
    }
    output.push_back(static_cast<char32_t>(codepoint));
    offset += static_cast<std::size_t>(consumed);
  }
#endif
  return true;
}

inline bool validate(std::string_view value) {
#if TEXT_LIBRARY_BACKEND == 4
  return asobmashow::text::validUtf8(value);
#endif
  if (value.empty()) return true;
#if TEXT_LIBRARY_BACKEND == 2
  return simdutf::validate_utf8(value.data(), value.size());
#elif TEXT_LIBRARY_BACKEND == 3
  return utf8::is_valid(value.begin(), value.end());
#else
  for (std::size_t offset = 0; offset < value.size();) {
    utf8proc_int32_t codepoint = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t *>(value.data() + offset),
        static_cast<utf8proc_ssize_t>(value.size() - offset), &codepoint);
    if (consumed <= 0) return false;
    offset += static_cast<std::size_t>(consumed);
  }
  return true;
#endif
}

} // namespace asobmashow::text_library_experiment
