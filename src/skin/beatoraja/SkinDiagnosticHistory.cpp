#include "SkinDiagnosticHistory.h"

#include "../package/SkinPackageCatalog.h"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace skin {

struct SkinDiagnosticHistory::Impl {
  explicit Impl(SkinPackageCatalog &owner) : catalog(owner) {
    values = catalog.loadDiagnosticHistory();
    if (!values.empty()) {
      const std::uint64_t maximum = values.back().recordSerial;
      nextRecordSerial =
          maximum == std::numeric_limits<std::uint64_t>::max() ? 0 : maximum + 1;
    }
    worker = std::thread([this] { run(); });
  }

  ~Impl() { stop(); }

  void run() noexcept {
    std::unique_lock lock(mutex);
    while (true) {
      changed.wait(lock, [this] { return closing || pending; });
      if (closing && !pending) {
        break;
      }
      pending = false;
      const std::uint64_t generation = submittedGeneration;
      std::vector<SkinDiagnosticHistoryRecord> replacement;
      try {
        replacement = values;
      } catch (...) {
        completedGeneration = std::max(completedGeneration, generation);
        completed.notify_all();
        continue;
      }
      writing = true;
      lock.unlock();
      try {
        (void)catalog.replaceDiagnosticHistory(replacement);
      } catch (...) {
      }
      lock.lock();
      completedGeneration = std::max(completedGeneration, generation);
      writing = false;
      completed.notify_all();
    }
  }

  void stop() noexcept {
    std::call_once(stopOnce, [this] {
      {
        std::scoped_lock lock(mutex);
        closing = true;
        changed.notify_all();
      }
      if (worker.joinable()) {
        worker.join();
      }
    });
  }

  SkinPackageCatalog &catalog;
  std::mutex mutex;
  std::condition_variable changed;
  std::condition_variable completed;
  std::vector<SkinDiagnosticHistoryRecord> values;
  std::uint64_t nextRecordSerial = 1;
  std::uint64_t submittedGeneration = 0;
  std::uint64_t completedGeneration = 0;
  bool pending = false;
  bool writing = false;
  bool closing = false;
  std::once_flag stopOnce;
  std::thread worker;
};

namespace {

void trimHistory(std::vector<SkinDiagnosticHistoryRecord> &records) {
  std::map<SkinEntryId, std::size_t> perEntry;
  std::vector<SkinDiagnosticHistoryRecord> retainedReverse;
  retainedReverse.reserve(records.size());
  for (auto iterator = records.rbegin(); iterator != records.rend(); ++iterator) {
    std::size_t &count = perEntry[iterator->entry];
    if (count++ < SkinDiagnosticHistory::maxRecordsPerEntry) {
      retainedReverse.push_back(std::move(*iterator));
    }
  }
  std::reverse(retainedReverse.begin(), retainedReverse.end());
  if (retainedReverse.size() > SkinDiagnosticHistory::maxGlobalRecords) {
    retainedReverse.erase(
        retainedReverse.begin(),
        retainedReverse.end() - SkinDiagnosticHistory::maxGlobalRecords);
  }
  records = std::move(retainedReverse);
}

} // namespace

SkinDiagnosticHistory::SkinDiagnosticHistory(SkinPackageCatalog &catalog)
    : impl_(std::make_unique<Impl>(catalog)) {}

SkinDiagnosticHistory::~SkinDiagnosticHistory() = default;

void SkinDiagnosticHistory::append(SkinDiagnosticHistoryRecord record) {
  std::scoped_lock lock(impl_->mutex);
  if (impl_->closing || impl_->nextRecordSerial == 0) {
    return;
  }
  record.recordSerial = impl_->nextRecordSerial;
  impl_->nextRecordSerial =
      record.recordSerial == std::numeric_limits<std::uint64_t>::max()
          ? 0
          : record.recordSerial + 1;
  impl_->values.push_back(std::move(record));
  trimHistory(impl_->values);
  ++impl_->submittedGeneration;
  impl_->pending = true;
  impl_->changed.notify_one();
}

std::vector<SkinDiagnosticHistoryRecord> SkinDiagnosticHistory::records() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->values;
}

std::vector<SkinDiagnosticHistoryRecord>
SkinDiagnosticHistory::recordsFor(const SkinEntryId &entry) const {
  std::scoped_lock lock(impl_->mutex);
  std::vector<SkinDiagnosticHistoryRecord> result;
  for (const auto &record : impl_->values) {
    if (record.entry == entry) {
      result.push_back(record);
    }
  }
  return result;
}

void SkinDiagnosticHistory::flush() {
  std::unique_lock lock(impl_->mutex);
  const std::uint64_t target = impl_->submittedGeneration;
  impl_->completed.wait(lock, [this, target] {
    return impl_->completedGeneration >= target ||
           (impl_->closing && !impl_->pending && !impl_->writing);
  });
}

} // namespace skin
