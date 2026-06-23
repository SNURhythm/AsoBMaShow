#pragma once

#if TARGET_OS_ANDROID

#include "ThreadCompat.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

std::string GetAndroidExternalFilesDir();
std::string GetAndroidInternalFilesDir();
bool PickAndroidChartFolder(std::filesystem::path &rootPath,
                            std::string &treeUri,
                            std::string &errorMessage);
bool PickAndroidArchiveForImport(std::filesystem::path &archivePath,
                                 std::string &errorMessage);
std::optional<std::filesystem::path>
ConsumePendingAndroidArchiveImport(std::string &errorMessage);
void RegisterAndroidChartFolder(const std::filesystem::path &rootPath,
                                const std::string &treeUri);
bool IsAndroidTreePath(const std::filesystem::path &path);
bool ExistsAndroidTreeFile(const std::filesystem::path &path,
                           std::string &errorMessage);
bool ListAndroidTreeChartFiles(const std::filesystem::path &rootPath,
                               std::vector<std::filesystem::path> &chartPaths,
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
void RequestAndroidExternalActivityRenderPause();
void FinishAndroidExternalActivityRenderPause();
bool IsAndroidExternalActivityRenderPauseRequested();
void NotifyAndroidExternalActivityRenderPaused();

#endif
