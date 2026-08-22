#include "LuaSkinJavaPattern.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(__APPLE__) || !TARGET_OS_IPHONE

#include "LuaSkinJavaPatternFlags.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <cstdint>
#include <utility>

namespace skin {

struct LuaSkinJavaPattern::Impl {
  explicit Impl(pcre2_code *value) noexcept : code(value) {}
  ~Impl() { pcre2_code_free(code); }

  pcre2_code *code = nullptr;
};

LuaSkinJavaPattern::LuaSkinJavaPattern(
    std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

std::optional<LuaSkinJavaPattern>
LuaSkinJavaPattern::compile(std::string_view pattern) {
  try {
    auto adapted =
        lua_skin_java_pattern_detail::adaptJavaEmbeddedFlags(pattern);
    const std::uint32_t options = PCRE2_UTF | PCRE2_ALT_BSUX;
    std::unique_ptr<pcre2_compile_context,
                    decltype(&pcre2_compile_context_free)>
        context(pcre2_compile_context_create(nullptr),
                &pcre2_compile_context_free);
    if (!context ||
        pcre2_set_max_pattern_length(context.get(), 65536) != 0) {
      return std::nullopt;
    }
    int error = 0;
    PCRE2_SIZE offset = 0;
    pcre2_code *code = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(adapted.expression.data()),
        adapted.expression.size(), options, &error, &offset, context.get());
    if (code == nullptr) {
      return std::nullopt;
    }
    return LuaSkinJavaPattern(std::make_shared<Impl>(code));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string>
LuaSkinJavaPattern::find(std::string_view subject) const noexcept {
  try {
    if (!impl_ || impl_->code == nullptr) {
      return std::nullopt;
    }
    std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>
        match(pcre2_match_data_create_from_pattern(impl_->code, nullptr),
              &pcre2_match_data_free);
    std::unique_ptr<pcre2_match_context,
                    decltype(&pcre2_match_context_free)>
        context(pcre2_match_context_create(nullptr), &pcre2_match_context_free);
    if (!match || !context ||
        pcre2_set_match_limit(context.get(), 100000) != 0 ||
        pcre2_set_depth_limit(context.get(), 1000) != 0 ||
        pcre2_set_heap_limit(context.get(), 8192) != 0) {
      return std::nullopt;
    }
    const int result = pcre2_match(
        impl_->code, reinterpret_cast<PCRE2_SPTR>(subject.data()),
        subject.size(), 0, 0, match.get(), context.get());
    if (result < 0) {
      return std::nullopt;
    }
    const PCRE2_SIZE *offsets = pcre2_get_ovector_pointer(match.get());
    if (offsets == nullptr || offsets[0] == PCRE2_UNSET ||
        offsets[1] == PCRE2_UNSET || offsets[0] > offsets[1] ||
        offsets[1] > subject.size()) {
      return std::nullopt;
    }
    return std::string(subject.substr(offsets[0], offsets[1] - offsets[0]));
  } catch (...) {
    return std::nullopt;
  }
}

} // namespace skin

#endif
