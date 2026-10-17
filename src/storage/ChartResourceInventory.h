#pragma once

#include "StorageTypes.h"
#include "../audio/ChartVisualAssetResolver.h"

#include <map>
#include <set>

namespace storage_management {

enum class StorageSourceKind { Local, Archive, Unsupported };
struct FolderSelection {
  std::string groupId;
  std::filesystem::path path;
  bool writableLocal = true;
  bool chartPathFallback = false;
  StorageSourceKind sourceKind = StorageSourceKind::Local;
};
struct InventoryRequest {
  std::vector<FolderSelection> groups;
  StoragePolicy policy;
  bool isolatedStaging = false;
  // Trusted sandbox/bookmark boundaries; empty retains the full filesystem walk.
  std::vector<std::filesystem::path> pathAccessRoots;
  std::filesystem::path scanScratchRoot;
  // Private app sandbox boundary for scratch files, separate from library access.
  std::filesystem::path scanAccessRoot;
};
enum class AssetOperation { LosslessFlac, LossyVorbis, CompressH264, Delete };
struct AssetCandidate {
  std::filesystem::path source;
  std::filesystem::path destination;
  asobmshow::chart_assets::MediaKind kind = asobmshow::chart_assets::MediaKind::Other;
  AssetOperation operation = AssetOperation::LosslessFlac;
  std::vector<std::string> groups;
  FileIdentity identity;
  std::string sourceHash; // Filled immediately before encoding, not during browsing.
};
struct GroupStorageMeasurement {
  std::string groupId;
  std::filesystem::path root;
  std::optional<std::uint64_t> totalBytes;
  std::uint64_t audioBytes = 0;
  std::uint64_t videoBytes = 0;
  std::uint64_t imageBytes = 0;
  StorageDiagnostic reason;
  StorageSourceKind sourceKind = StorageSourceKind::Local;
  std::string sourceIdentity;
  std::uint64_t generation = 0;
};
struct StorageInventory {
  InventoryRequest request;
  std::vector<AssetCandidate> candidates;
  std::vector<GroupStorageMeasurement> groups;
  std::vector<FileResult> skips;
  std::map<std::filesystem::path, FileIdentity> files;
  // Includes unsafe entries without following or reading their targets.
  std::set<std::filesystem::path> entries;
  std::map<std::filesystem::path, std::filesystem::file_time_type> directories;
  std::optional<std::uint64_t> totalBytes;
  bool complete = true;
  bool cancelled = false;
};

// Rejects symlink components rather than canonicalizing through them.
bool IsSafeLocalPath(const std::filesystem::path &path, std::string &error,
                     const std::vector<std::filesystem::path> &accessRoots = {});
std::optional<FileIdentity> ReadFileIdentity(const std::filesystem::path &path,
                                           std::string &error,
                                           const std::vector<std::filesystem::path> &accessRoots = {});
bool IsWithinFolder(const std::filesystem::path &path, const std::filesystem::path &folder);
StorageInventory BuildInventory(const InventoryRequest &, Checkpoint = {}, ProgressCallback = {});
bool RevalidateInventory(const StorageInventory &, Checkpoint, std::string &error);

} // namespace storage_management
