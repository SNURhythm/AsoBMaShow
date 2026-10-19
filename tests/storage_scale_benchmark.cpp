// Opt-in benchmark: every invocation owns disposable fixtures and prints one CSV row.
#include "StorageTestSupport.h"
#include "../src/storage/StorageMeasurement.h"
#include "../src/storage/StorageOptimizer.h"
#include "../src/storage/AudioTranscoder.h"
#include "../src/storage/StorageArchiveTransaction.h"
#include "../src/ArchiveFile.h"
#include "../src/ArchiveRAII.h"
#include <archive_entry.h>
#include <sndfile.h>
#include <atomic>
#include <barrier>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <thread>
#include <sys/resource.h>

using namespace storage_management;
using storage_test::require;
using storage_test::TempDirectory;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
double peakMiB() {
  rusage usage{}; getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
  return usage.ru_maxrss / 1048576.;
#else
  return usage.ru_maxrss / 1024.;
#endif
}
void writeArchive(const fs::path &path, std::size_t groups, std::size_t files,
    std::size_t payloadBytes = 17, bool update = false) {
  auto writer = makeArchiveWriteHandle();
  require(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK, "zip format");
  if (update) require(archive_write_set_options(writer.get(), "zip:compression=store") == ARCHIVE_OK, "stored fixture");
  require(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK, "zip open");
  const std::string payload(payloadBytes, 'a');
  for (std::size_t g = 0; g < groups; ++g) for (std::size_t f = 0; f < files; ++f) {
    auto *entry = archive_entry_new();
    const auto name = "g" + std::to_string(g) + "/a" + std::to_string(f) +
        (update ? (g == 0 && f == 0 ? ".avi" : ".png") : ".wav");
    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_filetype(entry, AE_IFREG); archive_entry_set_perm(entry, 0644); archive_entry_set_size(entry, payloadBytes);
    require(archive_write_header(writer.get(), entry) == ARCHIVE_OK, "zip header");
    require(archive_write_data(writer.get(), payload.data(), payload.size()) == static_cast<la_ssize_t>(payload.size()), "zip data");
    archive_entry_free(entry);
  }
  require(archive_write_close(writer.get()) == ARCHIVE_OK, "zip close");
}
void writeAudio(const fs::path &path, bool flac) {
  SF_INFO info{}; info.channels = 2; info.samplerate = 44100;
  info.format = (flac ? SF_FORMAT_FLAC : SF_FORMAT_WAV) | SF_FORMAT_PCM_16;
  auto *file = sf_open(path.c_str(), SFM_WRITE, &info);
  require(file != nullptr, "audio fixture open");
  std::vector<short> samples(44100);
  // Half a second of deterministic, moderately complex stereo audio.
  for (std::size_t i = 0; i < samples.size(); ++i)
    samples[i] = static_cast<short>(7000 * std::sin(i * .061) + 4000 * std::sin(i * .137));
  require(sf_writef_short(file, samples.data(), samples.size() / 2) == 22050, "audio fixture write");
  require(sf_close(file) == 0, "audio fixture close");
}
int main(int argc, char **argv) {
  try {
    require(argc >= 5, "usage: storage_scale_benchmark MODE DIRECTORIES FILES_PER_DIRECTORY WORKERS [LAYOUT_OR_CACHE_OR_CODEC]");
    const std::string mode = argv[1], variant = argc > 5 ? argv[5] : "roots";
    const auto dirs = std::stoull(argv[2]), files = std::stoull(argv[3]), workers = std::stoull(argv[4]);
    require(dirs && workers && workers <= 32, "positive directories/workers, at most 32 workers");
    TempDirectory temp;
    const auto root = temp.root / "library";
    fs::create_directory(root);
    InventoryRequest request;
    StorageScanDependencies deps;
    StorageScanScheduleMetrics schedule;
    StorageScanMetrics metrics;
    deps.limits.workers = workers; deps.limits.maxSubmitted = workers * 2;
    deps.limits.archiveBuilders = std::max<std::size_t>(1, workers / 2);
    deps.limits.metrics = &schedule; deps.limits.scratchRoot = temp.root / "scan";
    fs::create_directory(deps.limits.scratchRoot);
    std::atomic<std::uint64_t> enumerationNs{0};
    double encodeSeconds = 0, verifySeconds = 0, commitSeconds = 0;
    std::uint64_t converted = 0;
    StorageEvidenceMetrics evidence;
    const bool synthetic = mode == "synthetic" || mode == "cancel" || mode == "dispatch";
    const bool archiveReview = mode == "archive-review";
    const bool archiveUpdate = mode == "archive-update";
    const bool archive = mode == "archive" || mode == "archives" || archiveReview || archiveUpdate;
    if (archive) {
      archive_file::setArchiveIndexCacheDirectory(temp.root / "indexes");
      const auto count = mode == "archives" ? dirs : 1;
      const auto groups = mode == "archives" ? 1 : dirs;
      for (std::size_t a = 0; a < count; ++a) {
        const auto path = root / ("p" + std::to_string(a) + ".zip");
        writeArchive(path, groups, files, archiveUpdate ? std::stoull(variant) : 17, archiveUpdate);
        for (std::size_t g = 0; g < groups; ++g)
          request.groups.push_back({.groupId = std::to_string(a) + ":" + std::to_string(g),
              .path = path / ("g" + std::to_string(g)), .writableLocal = false, .sourceKind = StorageSourceKind::Archive});
      }
      if (archiveUpdate) require(MeasureStorageGroups(request).complete, "prime update index");
      else if (variant == "persisted") require(MeasureStorageGroups(request).complete, "prime persisted indexes");
      else if (variant == "resident") {
        for (std::size_t a = 0; a < count; ++a) {
          std::vector<archive_file::Entry> entries;
          require(archive_file::listEntries(root / ("p" + std::to_string(a) + ".zip"), entries), "prime resident indexes");
        }
      } else require(variant == "cold", "archive cache is cold/persisted/resident");
    } else {
      for (std::size_t d = 0; d < dirs; ++d) {
        const auto path = root / ("d" + std::to_string(d));
        request.groups.push_back({std::to_string(d), path});
        if (!synthetic) {
          fs::create_directory(path);
          for (std::size_t f = 0; f < files; ++f) {
            if (mode == "audio") writeAudio(path / ("a" + std::to_string(f) + (variant == "flac-vorbis" ? ".flac" : ".wav")), variant == "flac-vorbis");
            else temp.write(path.lexically_relative(temp.root) / ("a" + std::to_string(f) + ".wav"), "12345678901234567");
          }
        }
      }
      if (variant == "wide" || variant == "skewed") request.groups.insert(request.groups.begin(), {"root", root});
    }
    if (synthetic) {
      deps.enumerate = [&](const fs::path &path, const auto &, auto, auto check, auto visit, auto &stamp, auto *counts, auto &) {
        const auto start = Clock::now();
        stamp.fileId = 1;
        bool ok = true;
        auto emit = [&](StorageDirectoryEntry entry) {
          if (check && !check()) return false;
          if (counts) { ++counts->entries; ++counts->metadataReads; }
          return visit(entry);
        };
        if (path == root) {
          for (std::size_t d = 0; d < dirs && ok; ++d)
            ok = emit({.name = "d" + std::to_string(d), .kind = StorageEntryKind::Directory});
        } else {
          const auto count = mode == "dispatch" ? 0 : variant == "skewed" ? (path.filename() == "d0" ? dirs * files : 0) : files;
          for (std::size_t f = 0; f < count && ok; ++f)
            ok = emit({.name = "a" + std::to_string(f) + ".wav", .kind = StorageEntryKind::Regular, .bytes = 17});
        }
        enumerationNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        return ok;
      };
    } else if (!archive) {
      deps.enumerate = [&](const auto &path, const auto &access, auto visitMode, auto check, auto visit, auto &stamp, auto *counts, auto &error) {
        const auto start = Clock::now();
        const bool ok = VisitStorageDirectory(path, access, visitMode, check, visit, stamp, counts, error);
        enumerationNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        return ok;
      };
    }
    const auto start = Clock::now();
    double preflightSeconds = 0;
    if (archiveUpdate) {
      request.policy.bga = BgaAction::Delete;
      request.scanScratchRoot = deps.limits.scratchRoot;
      const auto review = BuildStoragePreflight(request, {}, &metrics, deps);
      require(review.complete && review.eligibleCount == 1, "one archive deletion");
      preflightSeconds = seconds(start);
      auto ready = Clock::now();
      StorageArchiveDependencies updateDeps{.faultPoint = [&](std::string_view point) {
        if (point == "archive-ready") { verifySeconds = seconds(ready); ready = Clock::now(); }
      }};
      const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
          MakeStoragePlatformPaths(temp.root / "private", {root}), updateDeps);
      commitSeconds = seconds(ready);
      require(report.deleted == 1 && !report.failed && !report.recoveryRequired, "archive compaction completed");
    } else if (archiveReview) {
      request.policy.audio = AudioAction::LosslessFlac;
      request.scanScratchRoot = deps.limits.scratchRoot;
      const auto review = BuildStoragePreflight(request, {}, &metrics, deps);
      require(review.complete && review.eligibleCount == dirs * files, "archive review candidates");
      preflightSeconds = seconds(start);
    } else if (mode == "evidence") {
      require(dirs == workers, "evidence benchmark holds one directory per worker");
      request.policy.audio = AudioAction::LosslessFlac;
      auto preflight = BuildStoragePreflight(request, {}, &metrics, deps);
      require(preflight.complete && preflight.eligibleCount == dirs * files, "evidence preflight");
      preflightSeconds = seconds(start);
      std::barrier loaded(static_cast<std::ptrdiff_t>(workers));
      std::vector<std::jthread> threads;
      std::atomic<bool> valid{true};
      for (auto &[path, summary] : preflight.directories) threads.emplace_back([&, summaryPtr = &summary] {
        std::string error;
        StorageDirectoryEvidence active(*summaryPtr, preflight, {.scratchRoot = deps.limits.scratchRoot});
        if (!active.Load({}, error)) valid = false;
        loaded.arrive_and_wait(); // Keep all bounded live/candidate/chart indexes resident together.
      });
      threads.clear();
      require(valid, "all active evidence indexes verified");
    } else if (mode == "audio") {
      request.policy.audio = variant == "wav-flac" ? AudioAction::LosslessFlac : AudioAction::LossyVorbis;
      auto preflight = BuildStoragePreflight(request, {}, &metrics, deps);
      require(preflight.complete && preflight.eligibleCount == dirs * files, "audio preflight");
      preflightSeconds = seconds(start);
      auto paths = MakeStoragePlatformPaths(temp.root / "private", {root});
      auto optimizerDeps = DefaultOptimizerDependencies(paths);
      optimizerDeps.executionLimits.audioWorkers = workers; optimizerDeps.evidenceMetrics = &evidence;
      std::mutex timings;
      optimizerDeps.encodeAudio = [&](const auto &asset, const auto &policy, const auto &output, auto check, auto progress) {
        const auto begin = Clock::now(); auto verify = begin; bool verifying = false;
        auto result = EncodeAudio(asset, policy, output, check, [&](const auto &event) {
          if (event.phase == StoragePhase::Verify) { verify = Clock::now(); verifying = true; }
          if (progress) progress(event);
        });
        std::lock_guard lock(timings);
        encodeSeconds += std::chrono::duration<double>((verifying ? verify : Clock::now()) - begin).count();
        if (verifying) verifySeconds += seconds(verify);
        return result;
      };
      const auto factory = optimizerDeps.transactions;
      optimizerDeps.transactions = [&, factory](const auto &platform, auto condition) {
        auto transaction = factory(platform, condition);
        const auto replace = transaction.replace;
        transaction.replace = [&, replace](const auto &asset, const auto &encoded, auto check) {
          const auto begin = Clock::now();
          auto result = replace(asset, encoded, check);
          std::lock_guard lock(timings); commitSeconds += seconds(begin); return result;
        };
        return transaction;
      };
      auto report = StorageOptimizer(std::move(optimizerDeps)).Run({request, request.policy, std::move(preflight)});
      require(!report.failed && !report.recoveryRequired && !report.cancelled, "audio optimization completed");
      converted = report.converted;
      require(converted == dirs * files, "all audio fixtures must become smaller");
    } else {
      auto measured = MeasureStorageGroups(request, mode == "cancel" ? Checkpoint([&] { return seconds(start) < .1; }) : Checkpoint{}, {}, &metrics, deps);
      if (mode == "cancel") require(measured.cancelled, "cancelled measurement");
      else {
        require(measured.complete && measured.totalBytes == (mode == "dispatch" ? 0 : dirs * files * 17), "exact totals");
        require(measured.directories.size() <= dirs + 1, "directory-only summaries");
      }
    }
    const auto elapsed = seconds(start);
    require(schedule.peakSubmitted <= workers * 2 && schedule.peakFrontierRecords <= deps.limits.frontierRecords, "bounded scan admission/frontier");
    require(schedule.peakBufferedBatches <= workers * 2, "bounded result queue");
    require(!synthetic || peakMiB() < 512, "synthetic peak RSS below 512 MiB");
    std::cout << std::fixed << std::setprecision(6)
      << "mode,variant,dirs,files_per_dir,workers,wall_s,enumeration_worker_s,preflight_s,encode_worker_s,verify_worker_s,commit_worker_s,peak_mib,entries,metadata_reads,archive_visits,archive_entries,root_steps,submitted,batches,frontier,spilled,converted,membership_enumerations,membership_entries\n"
      << mode << ',' << variant << ',' << dirs << ',' << files << ',' << workers << ',' << elapsed << ','
      << enumerationNs / 1e9 << ',' << preflightSeconds << ',' << encodeSeconds << ',' << verifySeconds << ',' << commitSeconds << ','
      << peakMiB() << ',' << metrics.entries << ',' << metrics.metadataReads << ',' << metrics.archiveIndexVisits << ',' << metrics.archiveEntries << ','
      << metrics.rootIndexSteps << ',' << schedule.peakSubmitted << ',' << schedule.peakBufferedBatches << ',' << schedule.peakFrontierRecords << ','
      << schedule.spilledJobs << ',' << converted << ',' << evidence.membershipEnumerations << ',' << evidence.membershipEntries << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
