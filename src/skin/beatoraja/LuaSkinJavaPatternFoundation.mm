#include "LuaSkinJavaPattern.h"

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#if TARGET_OS_IPHONE || defined(ASOBMASHOW_TEST_FOUNDATION_JAVA_PATTERN)

#include "LuaSkinJavaPatternFlags.h"

#include <utility>

namespace skin {

struct LuaSkinJavaPattern::Impl {
  explicit Impl(NSRegularExpression *value) noexcept : expression(value) {}
  __strong NSRegularExpression *expression = nil;
};

LuaSkinJavaPattern::LuaSkinJavaPattern(
    std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

std::optional<LuaSkinJavaPattern>
LuaSkinJavaPattern::compile(std::string_view pattern) {
  @autoreleasepool {
    try {
      auto adapted =
          lua_skin_java_pattern_detail::adaptJavaEmbeddedFlags(pattern);
      NSString *source = [[NSString alloc]
          initWithBytes:adapted.expression.data()
                 length:adapted.expression.size()
               encoding:NSUTF8StringEncoding];
      if (source == nil) {
        return std::nullopt;
      }
      NSError *error = nil;
      NSRegularExpression *expression =
          [[NSRegularExpression alloc] initWithPattern:source
                                               options:adapted.unixLines
                                                           ? NSRegularExpressionUseUnixLineSeparators
                                                           : 0
                                                 error:&error];
      if (expression == nil || error != nil) {
        return std::nullopt;
      }
      return LuaSkinJavaPattern(std::make_shared<Impl>(expression));
    } catch (...) {
      return std::nullopt;
    }
  }
}

std::optional<std::string>
LuaSkinJavaPattern::find(std::string_view subject) const noexcept {
  @autoreleasepool {
    try {
      if (!impl_ || impl_->expression == nil) {
        return std::nullopt;
      }
      NSString *text = [[NSString alloc] initWithBytes:subject.data()
                                                length:subject.size()
                                              encoding:NSUTF8StringEncoding];
      if (text == nil) {
        return std::nullopt;
      }
      NSTextCheckingResult *match = [impl_->expression
          firstMatchInString:text
                     options:0
                       range:NSMakeRange(0, text.length)];
      if (match == nil || match.range.location == NSNotFound) {
        return std::nullopt;
      }
      NSString *group = [text substringWithRange:match.range];
      const char *utf8 = group.UTF8String;
      if (utf8 == nullptr) {
        return std::nullopt;
      }
      return std::string(utf8);
    } catch (...) {
      return std::nullopt;
    }
  }
}

} // namespace skin

#endif
