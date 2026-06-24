#pragma once

#include "RAII.h"
#include "targets.h"

#include <curl/curl.h>

using CurlEasyHandle = UniqueResource<CURL, curl_easy_cleanup>;

inline void ConfigureCurlTrustStore(CURL *curl) {
  if (curl == nullptr) {
    return;
  }
#if TARGET_OS_ANDROID
  curl_easy_setopt(curl, CURLOPT_CAPATH, "/system/etc/security/cacerts");
#endif
}
