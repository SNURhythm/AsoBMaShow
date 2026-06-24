#include "Internal.h"
#include "GoogleDriveDriver.h"

#include "../RAII.h"

#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#endif
#if !(TARGET_OS_IOS || TARGET_OS_SIMULATOR)
#include "../CurlRAII.h"
#endif

namespace asobmshow::bms_search {

std::optional<std::filesystem::path> saveIosDebugArtifacts(
    const std::string &key, const std::string &downloadUrl,
    const std::string &displayUrl, const std::filesystem::path &archivePath,
    const std::filesystem::path &extractDirectory, std::string &errorMessage) {
#if (TARGET_OS_IOS || TARGET_OS_SIMULATOR) && defined(DEBUG)
  const std::string attemptId =
      key + "-" +
      std::to_string(
          std::chrono::system_clock::now().time_since_epoch().count());
  const std::filesystem::path debugDirectory =
      Utils::GetDocumentsPath("BMSSEARCH_DEBUG") / attemptId;
  std::error_code fsError;
  std::filesystem::create_directories(debugDirectory, fsError);
  if (fsError) {
    errorMessage = "Could not create debug folder: " + fsError.message();
    return std::nullopt;
  }

  if (!archivePath.empty() && std::filesystem::exists(archivePath, fsError)) {
    fsError.clear();
    std::filesystem::copy_file(
        archivePath, debugDirectory / archivePath.filename(),
        std::filesystem::copy_options::overwrite_existing, fsError);
    if (fsError) {
      errorMessage = "Could not copy downloaded archive: " + fsError.message();
      return std::nullopt;
    }
    writeArchiveEntryDiagnostics(archivePath,
                                 debugDirectory / "archive_entries.txt");
  }
  fsError.clear();

  if (!extractDirectory.empty() &&
      std::filesystem::exists(extractDirectory, fsError)) {
    fsError.clear();
    std::filesystem::copy(
        extractDirectory, debugDirectory / "extracted",
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing,
        fsError);
    if (fsError) {
      errorMessage = "Could not copy extracted files: " + fsError.message();
      return std::nullopt;
    }
  }
  fsError.clear();

  std::ofstream metadata(debugDirectory / "metadata.txt");
  if (!metadata) {
    errorMessage = "Could not create debug metadata.";
    return std::nullopt;
  }
  metadata << "download_url=" << downloadUrl << '\n';
  metadata << "display_url=" << displayUrl << '\n';
  metadata << "archive_path="
           << path_t_to_utf8(fspath_to_path_t(archivePath)) << '\n';
  metadata << "extract_path="
           << path_t_to_utf8(fspath_to_path_t(extractDirectory)) << '\n';
  metadata.close();
  if (!metadata) {
    errorMessage = "Could not write debug metadata.";
    return std::nullopt;
  }

  return debugDirectory;
#else
  (void)key;
  (void)downloadUrl;
  (void)displayUrl;
  (void)archivePath;
  (void)extractDirectory;
  (void)errorMessage;
  return std::nullopt;
#endif
}

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
struct IOSDownloadProgressContext {
  BmsSearchDownloadProgressCallback *progressCallback = nullptr;
};

void reportIOSDownloadProgress(void *context, std::uint64_t downloadedBytes,
                               std::uint64_t totalBytes) {
  auto *progressContext =
      static_cast<IOSDownloadProgressContext *>(context);
  if (progressContext == nullptr ||
      progressContext->progressCallback == nullptr ||
      !*progressContext->progressCallback) {
    return;
  }
  (*progressContext->progressCallback)(
      {.message = "Downloading archive",
       .downloadedBytes = downloadedBytes,
       .totalBytes = totalBytes});
}

std::optional<std::string> fetchUrlText(const std::string &url,
                                        std::string &errorMessage) {
  std::string body;
  if (!DownloadURLTextIOS(url, body, errorMessage)) {
    return std::nullopt;
  }
  return body;
}

std::optional<std::string> postUrlText(const std::string &url,
                                       std::string &errorMessage) {
  std::string body;
  if (!PostURLTextIOS(url, body, errorMessage)) {
    return std::nullopt;
  }
  return body;
}

bool downloadUrlToFile(const std::string &url, const std::filesystem::path &path,
                       std::atomic_bool &cancelled, std::string &errorMessage,
                       BmsSearchDownloadProgressCallback progressCallback) {
  if (cancelled.load()) {
    errorMessage = "Download cancelled.";
    return false;
  }
  if (progressCallback) {
    progressCallback({.message = "Downloading archive"});
  }
  std::vector<unsigned char> data;
  IOSDownloadProgressContext progressContext{
      .progressCallback = &progressCallback};
  if (!DownloadURLBinaryIOS(url, data, errorMessage,
                            reportIOSDownloadProgress, &progressContext)) {
    return false;
  }
  if (cancelled.load()) {
    errorMessage = "Download cancelled.";
    return false;
  }
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    errorMessage = "Could not create downloaded archive.";
    return false;
  }
  file.write(reinterpret_cast<const char *>(data.data()),
             static_cast<std::streamsize>(data.size()));
  if (!file) {
    errorMessage = "Could not write downloaded archive.";
    return false;
  }
  if (progressCallback) {
    progressCallback({.message = "Download complete",
                      .downloadedBytes = data.size(),
                      .totalBytes = data.size()});
  }
  return true;
}
#else
std::once_flag curlInitFlag;

size_t appendCurlResponse(char *ptr, size_t size, size_t nmemb,
                          void *userdata) {
  const size_t byteCount = size * nmemb;
  auto *response = static_cast<std::string *>(userdata);
  response->append(ptr, byteCount);
  return byteCount;
}

std::optional<std::string> fetchUrlText(const std::string &url,
                                        std::string &errorMessage) {
#if TARGET_OS_ANDROID
  std::string body;
  if (!DownloadURLTextAndroid(url, body, errorMessage)) {
    if (errorMessage.empty()) {
      errorMessage = "Failed to download " + url;
    }
    return std::nullopt;
  }
  return body;
#else
  std::call_once(curlInitFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CurlEasyHandle curl(curl_easy_init());
  if (curl == nullptr) {
    errorMessage = "Failed to initialize HTTP client.";
    return std::nullopt;
  }

  std::string body;
  char curlError[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "AsoBMaShow");
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, appendCurlResponse);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, curlError);
  curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  ConfigureCurlTrustStore(curl.get());

  const CURLcode result = curl_easy_perform(curl.get());
  long statusCode = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &statusCode);

  if (result != CURLE_OK) {
    errorMessage = curlError[0] != '\0' ? curlError : curl_easy_strerror(result);
    return std::nullopt;
  }
  if (statusCode >= 400) {
    errorMessage = "HTTP " + std::to_string(statusCode) + " while downloading " +
                   url;
    return std::nullopt;
  }
  return body;
#endif
}

std::optional<std::string> postUrlText(const std::string &url,
                                       std::string &errorMessage) {
#if TARGET_OS_ANDROID
  std::string body;
  if (!PostURLTextAndroid(url, body, errorMessage)) {
    if (errorMessage.empty()) {
      errorMessage = "Failed to post " + url;
    }
    return std::nullopt;
  }
  return body;
#else
  std::call_once(curlInitFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CurlEasyHandle curl(curl_easy_init());
  if (curl == nullptr) {
    errorMessage = "Failed to initialize HTTP client.";
    return std::nullopt;
  }

  std::string body;
  char curlError[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "AsoBMaShow");
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, appendCurlResponse);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, curlError);
  curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  ConfigureCurlTrustStore(curl.get());

  const CURLcode result = curl_easy_perform(curl.get());
  long statusCode = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &statusCode);

  if (result != CURLE_OK) {
    errorMessage = curlError[0] != '\0' ? curlError : curl_easy_strerror(result);
    return std::nullopt;
  }
  if (statusCode >= 400) {
    errorMessage = "HTTP " + std::to_string(statusCode) + " while posting " +
                   url;
    return std::nullopt;
  }
  return body;
#endif
}

struct CurlDownloadContext {
  std::ofstream *file = nullptr;
  std::atomic_bool *cancelled = nullptr;
  BmsSearchDownloadProgressCallback *progressCallback = nullptr;
};

size_t writeCurlFile(char *ptr, size_t size, size_t nmemb, void *userdata) {
  const size_t byteCount = size * nmemb;
  auto *context = static_cast<CurlDownloadContext *>(userdata);
  if (context->cancelled != nullptr && context->cancelled->load()) {
    return 0;
  }
  context->file->write(ptr, static_cast<std::streamsize>(byteCount));
  return *context->file ? byteCount : 0;
}

int curlProgress(void *userdata, curl_off_t downloadTotal,
                 curl_off_t downloadNow, curl_off_t, curl_off_t) {
  auto *context = static_cast<CurlDownloadContext *>(userdata);
  if (context->cancelled != nullptr && context->cancelled->load()) {
    return 1;
  }
  if (context->progressCallback != nullptr && *context->progressCallback) {
    (*context->progressCallback)(
        {.message = "Downloading archive",
         .downloadedBytes = static_cast<std::uint64_t>(downloadNow),
         .totalBytes = static_cast<std::uint64_t>(downloadTotal)});
  }
  return 0;
}

bool downloadUrlToFile(const std::string &url, const std::filesystem::path &path,
                       std::atomic_bool &cancelled, std::string &errorMessage,
                       BmsSearchDownloadProgressCallback progressCallback) {
#if TARGET_OS_ANDROID
  if (cancelled.load()) {
    errorMessage = "Download cancelled.";
    return false;
  }
  if (progressCallback) {
    progressCallback({.message = "Downloading archive"});
  }
  auto androidProgressCallback = [&progressCallback](
                                     std::uint64_t downloadedBytes,
                                     std::uint64_t totalBytes) {
    if (progressCallback) {
      progressCallback({.message = "Downloading archive",
                        .downloadedBytes = downloadedBytes,
                        .totalBytes = totalBytes});
    }
  };
  if (!DownloadURLToFileAndroid(url, path, cancelled, androidProgressCallback,
                                errorMessage)) {
    if (errorMessage.empty()) {
      errorMessage = "Download failed.";
    }
    return false;
  }
  if (cancelled.load()) {
    errorMessage = "Download cancelled.";
    return false;
  }
  if (progressCallback) {
    std::error_code fsError;
    const auto size = std::filesystem::file_size(path, fsError);
    const auto byteCount = fsError ? 0 : static_cast<std::uint64_t>(size);
    progressCallback({.message = "Download complete",
                      .downloadedBytes = byteCount,
                      .totalBytes = byteCount});
  }
  return true;
#else
  std::call_once(curlInitFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CurlEasyHandle curl(curl_easy_init());
  if (curl == nullptr) {
    errorMessage = "Failed to initialize HTTP client.";
    return false;
  }

  std::ofstream file(path, std::ios::binary);
  if (!file) {
    errorMessage = "Could not create downloaded archive.";
    return false;
  }

  CurlDownloadContext context{.file = &file,
                              .cancelled = &cancelled,
                              .progressCallback = &progressCallback};
  char curlError[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "AsoBMaShow");
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 180L);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCurlFile);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &context);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, curlProgress);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &context);
  curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, curlError);
  curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  ConfigureCurlTrustStore(curl.get());

  const CURLcode result = curl_easy_perform(curl.get());
  long statusCode = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &statusCode);
  file.close();

  if (cancelled.load()) {
    errorMessage = "Download cancelled.";
    return false;
  }
  if (result != CURLE_OK) {
    errorMessage = curlError[0] != '\0' ? curlError : curl_easy_strerror(result);
    return false;
  }
  if (statusCode >= 400) {
    errorMessage = "HTTP " + std::to_string(statusCode) + " while downloading " +
                   url;
    return false;
  }
  return true;
#endif
}
#endif

std::filesystem::path makeDownloadDirectory(
    const std::filesystem::path &libraryRoot) {
  if (!libraryRoot.empty()) {
    return libraryRoot / "BMSSEARCH";
  }
  return Utils::GetDocumentsPath("BMS") / "BMSSEARCH";
}

bool downloadAndExtractArchive(
    const std::string &downloadUrl, const std::string &displayUrl,
    const std::string &archiveKey, const std::filesystem::path &libraryRoot,
    std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    BmsSearchResult &result, const std::string &suggestedArchiveName,
    bool *downloadedArchive) {
  if (downloadedArchive != nullptr) {
    *downloadedArchive = false;
  }
  result.downloadUrl = displayUrl.empty() ? downloadUrl : displayUrl;
  std::string archiveExtension = archiveExtensionFromUrl(result.downloadUrl);
  if (archiveExtension.empty()) {
    archiveExtension = archiveExtensionFromUrl(downloadUrl);
  }
  if (archiveExtension.empty()) {
    archiveExtension = archiveExtensionFromName(suggestedArchiveName);
  }
  if (archiveExtension.empty()) {
    archiveExtension = ".archive";
  }
  const std::string archiveName = preferredArchiveName(
      suggestedArchiveName, result.downloadUrl, downloadUrl, archiveKey,
      archiveExtension);
  const std::string key = storageKeyFromArchiveName(archiveName);
  const std::filesystem::path baseDirectory = makeDownloadDirectory(libraryRoot);
  const std::filesystem::path archiveDirectory = baseDirectory / "_archives";
  const std::filesystem::path extractDirectory = baseDirectory / key;
  std::error_code fsError;
  std::filesystem::create_directories(archiveDirectory, fsError);
  if (fsError) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Could not create download folder: " + fsError.message();
    return false;
  }

  const std::filesystem::path archivePath = archiveDirectory / archiveName;
  auto archiveCleanup = makeScopeExit([archivePath] {
    if (archivePath.empty()) {
      return;
    }

    std::error_code error;
    if (!std::filesystem::exists(archivePath, error) || error) {
      return;
    }
    error.clear();
    std::filesystem::remove(archivePath, error);
    if (error) {
      SDL_Log("Could not delete downloaded archive %s: %s",
              path_t_to_utf8(fspath_to_path_t(archivePath)).c_str(),
              error.message().c_str());
    }
  });
  if (progressCallback) {
    progressCallback({.message = "Downloading archive"});
  }

  std::string downloadError;
  if (!downloadUrlToFile(downloadUrl, archivePath, cancelled, downloadError,
                         progressCallback)) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        downloadError.empty() ? "Download failed." : downloadError;
    return false;
  }
  if (downloadedArchive != nullptr) {
    *downloadedArchive = true;
  }

  std::string driveWarningError;
  if (!GoogleDriveDriver::resolveWarningDownload(
          downloadUrl, result.downloadUrl, archivePath, cancelled,
          driveWarningError, progressCallback)) {
    std::string debugError;
    if (const auto debugPath = saveIosDebugArtifacts(
            key, downloadUrl, result.downloadUrl, archivePath, extractDirectory,
            debugError)) {
      result.debugPath = *debugPath;
    } else if (!debugError.empty()) {
      SDL_Log("Failed to save BMS Search debug artifacts: %s",
              debugError.c_str());
    }
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        driveWarningError.empty() ? "Google Drive download failed."
                                  : driveWarningError;
    return false;
  }

  if (htmlBodyFromDownloadedFile(archivePath)) {
    std::string debugError;
    if (const auto debugPath = saveIosDebugArtifacts(
            key, downloadUrl, result.downloadUrl, archivePath, extractDirectory,
            debugError)) {
      result.debugPath = *debugPath;
    } else if (!debugError.empty()) {
      SDL_Log("Failed to save BMS Search debug artifacts: %s",
              debugError.c_str());
    }
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        "Downloaded response was an HTML page instead of an archive.";
    return false;
  }

  if (progressCallback) {
    progressCallback({.message = "Extracting archive"});
  }

  std::string extractError;
  if (!extractDownloadedArchive(archivePath, extractDirectory, extractError,
                                progressCallback)) {
    std::string debugError;
    if (const auto debugPath = saveIosDebugArtifacts(
            key, downloadUrl, result.downloadUrl, archivePath, extractDirectory,
            debugError)) {
      result.debugPath = *debugPath;
    } else if (!debugError.empty()) {
      SDL_Log("Failed to save BMS Search debug artifacts: %s",
              debugError.c_str());
    }
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        extractError.empty() ? "Archive extraction failed." : extractError;
    return false;
  }

  const std::string normalizedArchiveKey = lowerCopy(trimCopy(archiveKey));
  const bool requiresHashMatch =
      isHexStringOfLength(normalizedArchiveKey, 64) ||
      isHexStringOfLength(normalizedArchiveKey, 32);
  const bool foundBmsFile = containsBmsFile(extractDirectory);
  if (requiresHashMatch) {
    std::string hashError;
    if (!findMatchingBmsChartByHash(extractDirectory, normalizedArchiveKey,
                                    hashError)) {
      std::string debugError;
      if (const auto debugPath = saveIosDebugArtifacts(
              key, downloadUrl, result.downloadUrl, archivePath,
              extractDirectory, debugError)) {
        result.debugPath = *debugPath;
      } else if (!debugError.empty()) {
        SDL_Log("Failed to save BMS Search debug artifacts: %s",
                debugError.c_str());
      }
      result.status = BmsSearchResult::Status::HashMismatch;
      result.outputPath = extractDirectory;
      result.message = hashError.empty()
                           ? "Archive did not contain the selected BMS chart."
                           : hashError;
      return true;
    }
  } else if (!foundBmsFile) {
    std::string debugError;
    if (const auto debugPath = saveIosDebugArtifacts(
            key, downloadUrl, result.downloadUrl, archivePath, extractDirectory,
            debugError)) {
      result.debugPath = *debugPath;
    } else if (!debugError.empty()) {
      SDL_Log("Failed to save BMS Search debug artifacts: %s",
              debugError.c_str());
    }
  }

  result.status = BmsSearchResult::Status::Downloaded;
  result.outputPath = extractDirectory;
  result.message =
      foundBmsFile ? "Downloaded and extracted BMS archive."
                   : "Archive extracted, but no BMS file was found.";
  return true;
}


} // namespace asobmshow::bms_search
