#include "../src/ChartLibraryScanner.h"
#include "../src/repositories/ChartRepository.h"
#include "../src/sqlite3.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-chart-scanner-" + std::to_string(nonce) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::filesystem::path writeChart(const std::filesystem::path &root,
                                 const std::string &name,
                                 const std::string &title) {
  std::filesystem::create_directories(root);
  const auto chartPath = root / (name + ".bms");
  {
    std::ofstream chart(chartPath);
    chart << "#PLAYER 1\n"
             "#GENRE Test\n"
             "#TITLE "
          << title
          << "\n#ARTIST AsoBMaShow Test\n"
             "#BPM 120\n"
             "#PLAYLEVEL 1\n"
             "#RANK 2\n"
             "#TOTAL 100\n"
             "#WAV01 sample.wav\n"
             "#00111:01\n";
  }
  {
    std::ofstream audio(root / "sample.wav", std::ios::binary);
  }
  return chartPath;
}

void testBasicNoOpAndDeleteScan() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto chartPath = writeChart(root, "sample", "Repository Scanner");

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());

  ChartLibraryScanner scanner;
  std::vector<ChartScanProgress> progress;
  const std::uint64_t beforeRevision = repository.GetLibraryRevision();
  const int changed = scanner.Scan(
      *session, {root}, nullptr,
      [&](const ChartScanProgress &value) { progress.push_back(value); });
  assert(changed == 1);
  assert(session->CountAllChartMeta() == 1);
  assert(repository.GetLibraryRevision() > beforeRevision);
  assert(!progress.empty());
  assert(progress.front().stage == ChartScanProgressStage::Preparing);

  const std::uint64_t stableRevision = repository.GetLibraryRevision();
  assert(scanner.Scan(*session, {root}) == 0);
  assert(repository.GetLibraryRevision() == stableRevision);

  std::filesystem::remove(chartPath);
  assert(scanner.Scan(*session, {root}) == 1);
  assert(session->CountAllChartMeta() == 0);
  assert(repository.GetLibraryRevision() > stableRevision);
}

void testStopAndPauseBeforeWork() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  writeChart(root, "sample", "Stopped Scanner");

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  std::stop_source stopped;
  stopped.request_stop();
  const auto stoppedToken = stopped.get_token();
  assert(scanner.Scan(*session, {root}, &stoppedToken) == 0);
  assert(session->CountAllChartMeta() == 0);

  assert(scanner.Scan(*session, {root}, nullptr, nullptr,
                      []() { return false; }) == 0);
  assert(session->CountAllChartMeta() == 0);
}

void testCheckpointResume() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  writeChart(root, "first", "First Scanner Chart");
  writeChart(root, "second", "Second Scanner Chart");

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  std::stop_source stop;
  const auto stopToken = stop.get_token();
  const int firstChanged = scanner.Scan(
      *session, {root}, &stopToken, nullptr, nullptr,
      []() -> std::uint64_t { return 1; },
      [&](std::uint64_t request) {
        assert(request == 1);
        stop.request_stop();
      });
  assert(firstChanged == 1);
  assert(session->CountAllChartMeta() == 1);
  assert(session->LoadScanSnapshot().checkpoint.has_value());

  assert(scanner.Scan(*session, {root}) == 1);
  assert(session->CountAllChartMeta() == 2);
  assert(!session->LoadScanSnapshot().checkpoint.has_value());
}

std::atomic_bool denyChartInsert{false};

int denyChartInsertAuthorizer(void *, int action, const char *first,
                              const char *, const char *, const char *) {
  if (denyChartInsert.load(std::memory_order_relaxed) &&
      action == SQLITE_INSERT && first != nullptr &&
      std::string_view(first) == "chart_meta") {
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}

int installDenyChartInsert(sqlite3 *database, char **,
                           const sqlite3_api_routines *) {
  return sqlite3_set_authorizer(database, denyChartInsertAuthorizer, nullptr);
}

class ScopedInsertDenial {
public:
  ScopedInsertDenial() {
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(
               installDenyChartInsert)) == SQLITE_OK);
  }

  ~ScopedInsertDenial() {
    denyChartInsert.store(false, std::memory_order_relaxed);
    sqlite3_reset_auto_extension();
  }
};

void testStorageFailureLeavesNoChart() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  writeChart(root, "sample", "Denied Scanner");
  ScopedInsertDenial denial;

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  denyChartInsert.store(true, std::memory_order_relaxed);

  ChartLibraryScanner scanner;
  assert(scanner.Scan(*session, {root}) == 0);
  assert(session->CountAllChartMeta() == 0);
}

} // namespace

int main() {
  testBasicNoOpAndDeleteScan();
  testStopAndPauseBeforeWork();
  testCheckpointResume();
  testStorageFailureLeavesNoChart();
  return 0;
}
