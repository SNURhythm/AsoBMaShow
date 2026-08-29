#pragma once

#include "RAII.h"
#include "targets.h"

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

inline void ConfigureCurlTrustStore(CURL *curl) {
  if (curl == nullptr) {
    return;
  }
#if TARGET_OS_ANDROID
  curl_easy_setopt(curl, CURLOPT_CAPATH, "/system/etc/security/cacerts");
#endif
}
