#pragma once

#include <curl/curl.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace android_curl_trust {

inline constexpr std::size_t maximumFileBytes = 64 * 1024;
inline constexpr std::size_t maximumBundleBytes = 4 * 1024 * 1024;

// Android names roots using OpenSSL's old subject hash. Load their PEM data
// directly instead of asking modern OpenSSL to interpret that hashed CAPATH.
inline std::string loadBundle(const std::filesystem::path &apexDirectory,
                              const std::filesystem::path &systemDirectory) noexcept {
  try {
    std::error_code error;
    const bool apexExists = std::filesystem::exists(apexDirectory, error);
    if (error) return {};
    // An active but empty/unreadable APEX store must not resurrect roots removed
    // from it by falling back to (or merging in) the older system image store.
    const auto &directory = apexExists ? apexDirectory : systemDirectory;
    std::filesystem::directory_iterator files(directory, error);
    if (error) return {};
    std::string bundle;
    std::size_t totalBytes = 0;
    std::size_t entries = 0;
    std::array<char, maximumFileBytes + 1> buffer{};
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view end = "-----END CERTIFICATE-----";
    for (const auto &file : files) {
      if (++entries > 4096) return {};
      const auto status = file.symlink_status(error);
      if (error) return {};
      if (!std::filesystem::is_regular_file(status)) continue;
      std::ifstream input(file.path(), std::ios::binary);
      if (!input) return {};
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto bytes = static_cast<std::size_t>(input.gcount());
      if (input.bad() || !input.eof() || bytes > maximumFileBytes ||
          bytes > maximumBundleBytes - totalBytes) return {};
      totalBytes += bytes;
      const std::string_view contents(buffer.data(), bytes);
      std::size_t offset = 0;
      bool foundCertificate = false;
      while ((offset = contents.find(begin, offset)) != std::string_view::npos) {
        const auto last = contents.find(end, offset + begin.size());
        const auto nested = contents.find(begin, offset + begin.size());
        if (last == std::string_view::npos || nested < last) return {};
        const auto length = last + end.size() - offset;
        if (length + 1 > maximumBundleBytes - bundle.size()) return {};
        bundle.append(contents.substr(offset, length));
        bundle += '\n';
        foundCertificate = true;
        offset = last + end.size();
      }
      if (!foundCertificate) return {};
    }
    return bundle;
  } catch (...) {
    return {};
  }
}

// Callers must not perform a request on failure. COPY makes each easy handle
// independent of the bundle's storage, including asynchronous worker shutdown.
[[nodiscard]] inline bool configure(CURL *curl, std::string_view bundle) noexcept {
  if (curl == nullptr) return false;
  const bool cleared =
      curl_easy_setopt(curl, CURLOPT_CAINFO, static_cast<const char *>(nullptr)) == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_CAPATH, static_cast<const char *>(nullptr)) == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, static_cast<curl_blob *>(nullptr)) == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK;
  if (!cleared || bundle.empty() || bundle.size() > maximumBundleBytes) return false;
  curl_blob certificates{const_cast<char *>(bundle.data()), bundle.size(), CURL_BLOB_COPY};
  return curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &certificates) == CURLE_OK;
}

} // namespace android_curl_trust
