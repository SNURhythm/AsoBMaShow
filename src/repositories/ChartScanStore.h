#pragma once

#include "../bms_parser.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct ChartSourcePreference {
  int priority = 0;
  std::uint64_t archiveSize = 0;
};

struct SolidArchiveRecord {
  std::filesystem::path path;
};

struct SolidArchiveUpdate {
  std::filesystem::path path;
  std::uint64_t uncompressedSize = 0;
  int fileCount = 0;
};

struct ArchiveScanCacheRecord {
  std::filesystem::path path;
  std::int64_t archiveSize = 0;
  std::int64_t mtimeNs = 0;
  bool solid = false;
  std::uint64_t uncompressedSize = 0;
  int fileCount = 0;
  int chartCount = -1;
};

struct ArchiveScanCacheUpdate {
  std::filesystem::path path;
  bool solid = false;
  std::uint64_t uncompressedSize = 0;
  int fileCount = 0;
  int chartCount = 0;
};

struct ChartSourcePreferenceUpdate {
  std::filesystem::path path;
  int priority = 0;
  std::uint64_t archiveSize = 0;
};

struct ChartScanCheckpoint {
  bool found = false;
  std::string scanSignature;
  std::string phase;
  int nextIndex = 0;
  int subIndex = 0;
  std::filesystem::path lastPath;
  std::filesystem::path archivePath;
  std::int64_t archiveSize = 0;
  std::int64_t archiveMtimeNs = 0;
  std::string lastInnerPath;
};

struct CompletedArchiveRecord {
  std::filesystem::path path;
  std::int64_t size = 0;
  std::int64_t mtimeNs = 0;
};

struct ChartScanSnapshot {
  std::vector<bms_parser::ChartMeta> charts;
  std::vector<SolidArchiveRecord> solidArchives;
  std::vector<ArchiveScanCacheRecord> archiveCache;
  std::vector<CompletedArchiveRecord> completedArchives;
  std::optional<ChartScanCheckpoint> checkpoint;
};

enum class ChartScanSnapshotLoad {
  Full,
  CheckpointOnly,
  // Everything Full loads except the full chart metadata rows: solid archives,
  // archive scan cache, completed archives, and the checkpoint. Used by the
  // scanner, which reconciles stored charts from the lightweight identity
  // records instead of materializing every column for the whole library.
  Reconcile,
};

// Minimal stored-chart identity used by the scanner's reconcile pass. Only the
// identity columns (path + md5 + sha256) are loaded, avoiding the cost of
// materializing the full metadata row for every chart in the library.
struct ChartScanReconcileIdentity {
  std::filesystem::path path;
  std::string md5;
  std::string sha256;
};
