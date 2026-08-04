#include "skin/beatoraja/SkinDiagnosticHistory.h"
#include "skin/package/SkinPackageCatalog.h"
#include "skin/package/SkinPathPolicy.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    root_ = fs::canonical(fs::temp_directory_path()) /
            ("asobmashow-skin-diagnostic-history-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++serial));
    fs::create_directories(root_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

SkinEntryId fixtureEntry(std::string_view name = "FixtureSkin") {
  const auto package = normalizePackageId(name);
  expect(package.package.has_value(), "diagnostic fixture package is valid");
  if (!package.package) {
    std::abort();
  }
  const auto entry = normalizeEntryPath(*package.package, "play/play7.luaskin");
  expect(entry.entry.has_value(), "diagnostic fixture entry is valid");
  if (!entry.entry) {
    std::abort();
  }
  return *entry.entry;
}

SkinDiagnosticHistoryRecord fixtureRecord(std::string_view marker,
                                          std::string_view entryName = "FixtureSkin") {
  return {.recordSerial = 1,
          .entry = fixtureEntry(entryName),
          .revisionDigest =
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          .configurationDigest =
              "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
          .phase = SkinDiagnosticPhase::Validation,
          .diagnostic = {.code = std::string(marker),
                         .message = "fixture diagnostic",
                         .virtualPath = "play/play7.luaskin",
                         .severity = DiagnosticSeverity::Warning},
          .luaLine = 17,
          .frameSerial = 99};
}

bool replaceOne(SkinPackageCatalog &catalog, SkinDiagnosticHistoryRecord record) {
  const std::vector<SkinDiagnosticHistoryRecord> records{std::move(record)};
  return catalog.replaceDiagnosticHistory(records);
}

void testAppendPublishesChronologicalValueSnapshots() {
  TempDirectory temp;
  SkinPackageCatalog catalog(temp.root());
  SkinDiagnosticHistory history(catalog);

  history.append(fixtureRecord("first"));
  history.append(fixtureRecord("second"));

  const auto records = history.records();
  expect(records.size() == 2, "append publishes both diagnostic records");
  expect(records.size() == 2 && records[0].recordSerial == 1 &&
             records[1].recordSerial == 2,
         "append assigns nonzero monotonic record serials");
  expect(records.size() == 2 && records[0].diagnostic.code == "first" &&
             records[1].diagnostic.code == "second",
         "append preserves chronological record order");

  const auto forEntry = history.recordsFor(fixtureEntry());
  expect(forEntry.size() == 2 && forEntry[1].diagnostic.code == "second",
         "recordsFor returns only the requested entry's value snapshot");
}

void testBoundsKeepNewestPerEntryAndGlobally() {
  TempDirectory temp;
  SkinPackageCatalog catalog(temp.root());
  SkinDiagnosticHistory history(catalog);

  for (int index = 0; index < 40; ++index) {
    history.append(fixtureRecord("per-entry-" + std::to_string(index)));
  }
  const auto perEntry = history.recordsFor(fixtureEntry());
  expect(perEntry.size() == SkinDiagnosticHistory::maxRecordsPerEntry,
         "history retains at most 32 records per exact entry");
  expect(perEntry.size() == SkinDiagnosticHistory::maxRecordsPerEntry &&
             perEntry.front().diagnostic.code == "per-entry-8" &&
             perEntry.back().diagnostic.code == "per-entry-39",
         "per-entry bound keeps the newest chronological records");

  for (int index = 0; index < 256; ++index) {
    history.append(fixtureRecord("global-" + std::to_string(index),
                                 "Skin" + std::to_string(index)));
  }
  const auto records = history.records();
  expect(records.size() == SkinDiagnosticHistory::maxGlobalRecords,
         "history retains at most 256 records globally");
  expect(records.size() == SkinDiagnosticHistory::maxGlobalRecords &&
             records.front().diagnostic.code == "global-0" &&
             records.back().diagnostic.code == "global-255",
         "global bound drops older records while preserving chronology");
}

void testCatalogDeepCopiesAndCoalescesHistoryReplacements() {
  TempDirectory temp;
  SkinPackageCatalog catalog(temp.root());
  std::vector<SkinDiagnosticHistoryRecord> caller{fixtureRecord("owned")};
  expect(catalog.replaceDiagnosticHistory(caller),
         "catalog accepts a valid history replacement");
  caller[0].diagnostic.code = "mutated";
  caller.clear();
  caller.shrink_to_fit();
  catalog.flush();

  const auto ownedBeforeCoalescing = catalog.loadDiagnosticHistory();
  expect(ownedBeforeCoalescing.size() == 1 &&
             ownedBeforeCoalescing.front().diagnostic.code == "owned",
         "catalog deep-copies replacement records before returning");

  expect(replaceOne(catalog, fixtureRecord("old")),
         "catalog accepts a coalescible older replacement");
  expect(replaceOne(catalog, fixtureRecord("new")),
         "catalog accepts a coalescible newer replacement");
  catalog.flush();

  const auto records = catalog.loadDiagnosticHistory();
  expect(records.size() == 1 && records.front().diagnostic.code == "new",
         "newest pending history replacement wins after coalescing");
}

void testCatalogRejectsAnOverfullPersistedEntryHistory() {
  TempDirectory temp;
  SkinPackageCatalog catalog(temp.root());
  std::vector<SkinDiagnosticHistoryRecord> records;
  records.reserve(SkinDiagnosticHistory::maxRecordsPerEntry + 1);
  for (std::uint64_t index = 0;
       index <= SkinDiagnosticHistory::maxRecordsPerEntry; ++index) {
    auto record = fixtureRecord("overfull-" + std::to_string(index));
    record.recordSerial = index + 1;
    records.push_back(std::move(record));
  }

  expect(!catalog.replaceDiagnosticHistory(records),
         "catalog rejects a replacement exceeding the per-entry history cap");
  expect(catalog.loadDiagnosticHistory().empty(),
         "a rejected overfull replacement leaves history fail-closed and empty");
}

void testHistoryReconstructsPersistedSafeFields() {
  TempDirectory temp;
  {
    SkinPackageCatalog catalog(temp.root());
    SkinDiagnosticHistory history(catalog);
    auto record = fixtureRecord("persisted");
    record.phase = SkinDiagnosticPhase::FrameFallback;
    record.luaLine = 321;
    record.frameSerial = 654;
    history.append(std::move(record));
    history.flush();
    catalog.flush();
    catalog.shutdown();
  }

  SkinPackageCatalog catalog(temp.root());
  SkinDiagnosticHistory history(catalog);
  const auto records = history.records();
  expect(records.size() == 1, "history reloads a persisted record");
  expect(records.size() == 1 && records.front().recordSerial == 1 &&
             records.front().phase == SkinDiagnosticPhase::FrameFallback &&
             records.front().diagnostic.code == "persisted" &&
             records.front().luaLine == 321 &&
             records.front().frameSerial == 654,
         "history reconstruction preserves its safe persisted fields");
}

void testCorruptHistoryFailsClosedWithoutCatalogDamage() {
  TempDirectory temp;
  {
    SkinPackageCatalog catalog(temp.root());
    SkinDiagnosticHistory history(catalog);
    history.append(fixtureRecord("before-corruption"));
    history.flush();
    catalog.flush();
  }
  {
    std::ofstream corrupt(temp.root() / "diagnostic-history.json",
                          std::ios::binary | std::ios::trunc);
    corrupt << "{ not valid history";
  }

  SkinPackageCatalog catalog(temp.root());
  SkinDiagnosticHistory history(catalog);
  expect(history.records().empty(),
         "malformed history is rejected as an empty fail-closed history");
  expect(catalog.snapshot()->entries.empty(),
         "malformed separate history does not damage the catalog snapshot");
}

} // namespace

int main() {
  testAppendPublishesChronologicalValueSnapshots();
  testBoundsKeepNewestPerEntryAndGlobally();
  testCatalogDeepCopiesAndCoalescesHistoryReplacements();
  testCatalogRejectsAnOverfullPersistedEntryHistory();
  testHistoryReconstructsPersistedSafeFields();
  testCorruptHistoryFailsClosedWithoutCatalogDamage();
  return failures == 0 ? 0 : 1;
}
