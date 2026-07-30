#pragma once

#include <cstdint>

namespace platform {

enum class PhotoAuthorizationStatus : std::uint8_t {
  NotDetermined,
  Restricted,
  Denied,
  Authorized,
  Limited,
};

enum class PhotoAuthorizationAction : std::uint8_t {
  Request,
  ExplainRestriction,
  OpenSettings,
  Proceed,
};

[[nodiscard]] constexpr PhotoAuthorizationAction
photoAuthorizationAction(PhotoAuthorizationStatus status) noexcept {
  switch (status) {
  case PhotoAuthorizationStatus::NotDetermined:
    return PhotoAuthorizationAction::Request;
  case PhotoAuthorizationStatus::Restricted:
    return PhotoAuthorizationAction::ExplainRestriction;
  case PhotoAuthorizationStatus::Denied:
    return PhotoAuthorizationAction::OpenSettings;
  case PhotoAuthorizationStatus::Authorized:
  case PhotoAuthorizationStatus::Limited:
    return PhotoAuthorizationAction::Proceed;
  }
  return PhotoAuthorizationAction::ExplainRestriction;
}

} // namespace platform
