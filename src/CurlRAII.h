#pragma once

#include "RAII.h"
#include "targets.h"
#if TARGET_OS_ANDROID
#include "AndroidCurlTrustStore.h"
#endif

#include <curl/curl.h>
#include <string_view>

using CurlEasyHandle = UniqueResource<CURL, curl_easy_cleanup>;

inline const char *
CurlRedirectProtocolsForInitialUrl(std::string_view url) {
  constexpr std::string_view httpPrefix = "http://";
  if (url.size() < httpPrefix.size()) {
    return "https";
  }
  for (std::size_t i = 0; i < httpPrefix.size(); ++i) {
    const char value = url[i] >= 'A' && url[i] <= 'Z'
                           ? static_cast<char>(url[i] - 'A' + 'a')
                           : url[i];
    if (value != httpPrefix[i]) {
      return "https";
    }
  }
  return "http,https";
}

[[nodiscard]] inline bool ConfigureCurlTrustStore(CURL *curl) noexcept {
  if (curl == nullptr) {
    return false;
  }
#if TARGET_OS_ANDROID
  // Intentionally process-lived: async workers can outlive other static
  // destructors. Each easy handle still owns its own COPY of this bounded data.
  static const std::string *const bundle = []() noexcept -> const std::string * {
    try {
      return new std::string(android_curl_trust::loadBundle(
          "/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts"));
    } catch (...) {
      return nullptr;
    }
  }();
  return android_curl_trust::configure(curl, bundle ? std::string_view(*bundle) : std::string_view{});
#else
  return true;
#endif
}
