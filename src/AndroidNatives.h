#pragma once

#if TARGET_OS_ANDROID

#include "ThreadCompat.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using AndroidDownloadProgressCallback =
    std::function<void(std::uint64_t downloadedBytes,
                       std::uint64_t totalBytes)>;

struct AndroidNativeMusicMetadata {
  std::string title;
  std::string artist;
  std::string album;
  std::string artworkPath;
  long long durationMicros = 0;
};

struct AndroidNativeMusicQueueItem {
  AndroidNativeMusicMetadata metadata;
  long long itemId = 0;
};

struct AndroidNativeMusicQueue {
  std::string title;
  std::vector<AndroidNativeMusicQueueItem> items;
  int currentIndex = -1;
};

struct AndroidNativeMusicState {
  bool loaded = false;
  bool playing = false;
  long long positionMicros = 0;
  long long durationMicros = 0;
};

std::string GetAndroidExternalFilesDir();
std::string GetAndroidInternalFilesDir();
std::string GetAndroidCacheDir();
std::optional<std::string> ConvertAndroidMs932ToUtf8(std::string_view value);
bool AndroidBuildHasManageExternalStorage();
bool PickAndroidChartFolder(std::filesystem::path &rootPath,
                            std::string &treeUri,
                            std::string &errorMessage);
bool PickAndroidArchiveForImport(std::filesystem::path &archivePath,
                                 std::string &errorMessage);
bool PickAndroidFolderForImport(std::filesystem::path &folderPath,
                                std::string &errorMessage);
std::optional<std::filesystem::path>
ConsumePendingAndroidArchiveImport(std::string &errorMessage);
bool RegisterAndroidDocumentHandoff(std::uint64_t operationToken,
                                    std::string &errorMessage);
void RetireAndroidDocumentHandoff(std::uint64_t operationToken);
bool RegisterAndroidDocumentCommit(std::uint64_t operationToken,
                                   std::function<bool()> commitHandler);
void UnregisterAndroidDocumentCommit(std::uint64_t operationToken);
std::string ImportAndroidDocument(std::uint64_t operationToken,
                                  const std::string &mimeType,
                                  std::uint64_t maxBytes);
std::string ExportAndroidDocument(std::uint64_t operationToken,
                                  const std::filesystem::path &localPath,
                                  const std::string &mimeType,
                                  const std::string &suggestedName,
                                  std::uint64_t maxBytes);
void CancelAndroidDocument(std::uint64_t operationToken);
bool ValidateAndroidTemporaryDocument(const std::filesystem::path &localPath,
                                      std::string &errorMessage);
bool CleanupAndroidTemporaryDocument(const std::filesystem::path &localPath,
                                     std::string &errorMessage);
void RegisterAndroidChartFolder(const std::filesystem::path &rootPath,
                                const std::string &treeUri);
bool IsAndroidTreePath(const std::filesystem::path &path);
bool ExistsAndroidTreeFile(const std::filesystem::path &path,
                           std::string &errorMessage);
struct AndroidTreeChartFile {
  std::filesystem::path path;
  bool hasDocument = false;
};
bool ListAndroidTreeChartFiles(const std::filesystem::path &rootPath,
                               std::vector<AndroidTreeChartFile> &chartFiles,
                               std::string &errorMessage,
                               const std::stop_token *stopToken = nullptr);
bool ClearAndroidTreeTransientFileCache(std::string &errorMessage);
bool CacheAndroidTreeDirectory(const std::filesystem::path &directoryPath,
                               std::string &errorMessage);
bool ReadAndroidTreeFile(const std::filesystem::path &path,
                         std::vector<unsigned char> &bytes,
                         std::string &errorMessage);
std::optional<int> OpenAndroidTreeFileDescriptor(const std::filesystem::path &path,
                                                 std::string &errorMessage);
bool OpenURLInAndroidBrowser(const std::string &url,
                             std::string &errorMessage);
bool DownloadURLTextAndroid(const std::string &url, std::string &body,
                            std::string &errorMessage);
bool PostURLTextAndroid(const std::string &url, std::string &body,
                        std::string &errorMessage);
bool DownloadURLToFileAndroid(const std::string &url,
                              const std::filesystem::path &path,
                              std::atomic_bool &cancelled,
                              AndroidDownloadProgressCallback progressCallback,
                              std::string &errorMessage);
bool LoadAndroidNativeMusicFile(const std::string &filePath,
                                const AndroidNativeMusicMetadata &metadata,
                                std::string &errorMessage);
bool UpdateAndroidNativeMusicMetadata(
    const AndroidNativeMusicMetadata &metadata, std::string &errorMessage);
bool UpdateAndroidNativeMusicQueue(const AndroidNativeMusicQueue &queue,
                                   std::string &errorMessage);
bool PlayAndroidNativeMusic(std::string &errorMessage);
bool PauseAndroidNativeMusic(std::string &errorMessage);
bool StopAndroidNativeMusic(std::string &errorMessage);
bool SeekAndroidNativeMusic(long long positionMicros,
                            std::string &errorMessage);
bool SetAndroidNativeMusicPlaybackRate(int percent, bool timeStretch,
                                       std::string &errorMessage);
AndroidNativeMusicState GetAndroidNativeMusicState();
void RequestAndroidExternalActivityRenderPause();
void FinishAndroidExternalActivityRenderPause();
bool IsAndroidExternalActivityRenderPauseRequested();
void NotifyAndroidExternalActivityRenderPaused();

#endif
