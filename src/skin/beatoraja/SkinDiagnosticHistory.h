#pragma once

#include "../package/SkinPackageTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skin {

class SkinPackageCatalog;

enum class SkinDiagnosticPhase : std::uint8_t {
  Import,
  Scan,
  Validation,
  Session,
  FrameFallback,
};

struct SkinDiagnosticHistoryRecord {
  std::uint64_t recordSerial = 0;
  SkinEntryId entry;
  std::string revisionDigest;
  std::string configurationDigest;
  SkinDiagnosticPhase phase = SkinDiagnosticPhase::Validation;
  SkinDiagnostic diagnostic;
  std::optional<std::uint32_t> luaLine;
  std::optional<std::uint64_t> frameSerial;
};

class SkinDiagnosticHistory {
public:
  static constexpr std::size_t maxGlobalRecords = 256;
  static constexpr std::size_t maxRecordsPerEntry = 32;

  explicit SkinDiagnosticHistory(SkinPackageCatalog &catalog);
  ~SkinDiagnosticHistory();

  SkinDiagnosticHistory(const SkinDiagnosticHistory &) = delete;
  SkinDiagnosticHistory &operator=(const SkinDiagnosticHistory &) = delete;

  void append(SkinDiagnosticHistoryRecord record);
  std::vector<SkinDiagnosticHistoryRecord> records() const;
  std::vector<SkinDiagnosticHistoryRecord>
  recordsFor(const SkinEntryId &entry) const;
  void flush();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skin
