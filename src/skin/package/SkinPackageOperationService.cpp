#include "SkinPackageOperationService.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace skin {
namespace {

std::atomic_uint64_t nextOperationTicket{1};
constexpr std::size_t maxRetainedOperations = 128;

std::uint64_t allocateOperationTicket() noexcept {
  auto current = nextOperationTicket.load(std::memory_order_relaxed);
  while (current != 0 && current != std::numeric_limits<std::uint64_t>::max()) {
    if (nextOperationTicket.compare_exchange_weak(current, current + 1,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
      return current;
    }
  }
  return 0;
}

SkinDiagnostic operationFailure(std::string message) {
  return {.code = "skin.package.operation.failed",
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

} // namespace

struct SkinPackageProgressMailbox::State {
  mutable std::mutex mutex;
  SkinProgress progress;
};

SkinPackageProgressMailbox::SkinPackageProgressMailbox(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}

SkinProgress SkinPackageProgressMailbox::snapshot() const noexcept {
  try {
    std::scoped_lock lock(state_->mutex);
    return state_->progress;
  } catch (...) {
    return {};
  }
}

void SkinPackageProgressMailbox::publish(SkinProgress progress) noexcept {
  std::scoped_lock lock(state_->mutex);
  state_->progress = progress;
}

SkinDeferredCleanup::SkinDeferredCleanup(std::function<void()> action)
    : action_(std::move(action)) {}

SkinDeferredCleanup::SkinDeferredCleanup(SkinDeferredCleanup &&) noexcept =
    default;

SkinDeferredCleanup &
SkinDeferredCleanup::operator=(SkinDeferredCleanup &&) noexcept = default;

SkinDeferredCleanup::~SkinDeferredCleanup() = default;

void SkinDeferredCleanup::run() noexcept {
  if (!action_) {
    return;
  }
  auto action = std::move(action_);
  try {
    action();
  } catch (...) {
  }
}

struct SkinPackageOperationService::Impl {
  struct PrepareArchiveRequest {
    std::filesystem::path zip;
    SkinPackageId package;
    SkinDeferredCleanup cleanup;
  };
  struct PrepareFolderRequest {
    std::filesystem::path folder;
    SkinPackageId package;
    SkinDeferredCleanup cleanup;
  };
  struct DiscardPreparedRequest {
    PreparedPackage prepared;
    SkinDeferredCleanup cleanup;
  };
  using RequestPayload =
      std::variant<PrepareArchiveRequest, PrepareFolderRequest,
                   DiscardPreparedRequest>;

  struct Request {
    std::uint64_t ticket = 0;
    std::shared_ptr<SkinPackageProgressMailbox> mailbox;
    std::stop_source stop;
    std::atomic_bool detached = false;
    RequestPayload payload;

    Request(std::uint64_t requestTicket,
            std::shared_ptr<SkinPackageProgressMailbox> requestMailbox,
            RequestPayload requestPayload)
        : ticket(requestTicket), mailbox(std::move(requestMailbox)),
          payload(std::move(requestPayload)) {}
  };

  SkinPackageStore &store;
  SkinEntryValidator &validator;
  std::mutex mutex;
  std::condition_variable workAvailable;
  bool closing = false;
  std::deque<std::shared_ptr<Request>> queued;
  std::unordered_map<std::uint64_t, std::shared_ptr<Request>> outstanding;
  std::unordered_map<std::uint64_t, SkinPackageOperationCompletion> completions;
  std::jthread worker;
  std::once_flag joined;

  Impl(SkinPackageStore &operationStore, SkinEntryValidator &operationValidator)
      : store(operationStore), validator(operationValidator),
        worker([this] { run(); }) {}

  ~Impl() { shutdown(); }

  SkinPackageOperationHandle enqueue(RequestPayload payload) {
    std::uint64_t ticket = 0;
    std::shared_ptr<SkinPackageProgressMailbox> mailbox;
    std::shared_ptr<Request> request;
    {
      std::scoped_lock lock(mutex);
      if (closing ||
          outstanding.size() + completions.size() >= maxRetainedOperations ||
          (ticket = allocateOperationTicket()) == 0) {
        return {};
      }
      mailbox = std::shared_ptr<SkinPackageProgressMailbox>(
          new SkinPackageProgressMailbox(
              std::make_shared<SkinPackageProgressMailbox::State>()));
      request = std::make_shared<Request>(ticket, mailbox, std::move(payload));
      outstanding.emplace(ticket, request);
      queued.push_back(std::move(request));
    }
    workAvailable.notify_one();
    return {.ticket = ticket, .progress = std::move(mailbox)};
  }

  void enqueueDiscard(PreparedPackage prepared, SkinDeferredCleanup cleanup) {
    auto request = std::make_shared<Request>(
        0, std::shared_ptr<SkinPackageProgressMailbox>{},
        RequestPayload(DiscardPreparedRequest{.prepared = std::move(prepared),
                                              .cleanup = std::move(cleanup)}));
    {
      std::scoped_lock lock(mutex);
      if (closing) {
        return;
      }
      queued.push_back(std::move(request));
    }
    workAvailable.notify_one();
  }

  void run() noexcept {
    for (;;) {
      std::shared_ptr<Request> request;
      {
        std::unique_lock lock(mutex);
        workAvailable.wait(lock, [this] { return closing || !queued.empty(); });
        if (queued.empty()) {
          return;
        }
        request = std::move(queued.front());
        queued.pop_front();
      }

      std::optional<SkinPackageOperationPayload> result;
      try {
        std::visit(
            [&](auto &operation) {
              using Operation = std::decay_t<decltype(operation)>;
              if constexpr (std::is_same_v<Operation, PrepareArchiveRequest>) {
                try {
                  auto prepared =
                      store.prepareArchive(operation.zip, operation.package,
                                           request->stop.get_token(),
                                           [mailbox = request->mailbox](
                                               const SkinProgress &progress) {
                                             mailbox->publish(progress);
                                           });
                  result.emplace(std::move(prepared));
                } catch (const std::exception &) {
                  PreparePackageResult failed;
                  failed.diagnostics.push_back(
                      operationFailure("skin archive preparation failed"));
                  result.emplace(std::move(failed));
                } catch (...) {
                  PreparePackageResult failed;
                  failed.diagnostics.push_back(
                      operationFailure("unknown prepare error"));
                  result.emplace(std::move(failed));
                }
              } else if constexpr (std::is_same_v<Operation,
                                                  PrepareFolderRequest>) {
                try {
                  auto prepared =
                      store.prepareFolder(operation.folder, operation.package,
                                          request->stop.get_token(),
                                          [mailbox = request->mailbox](
                                              const SkinProgress &progress) {
                                            mailbox->publish(progress);
                                          });
                  result.emplace(std::move(prepared));
                } catch (const std::exception &) {
                  PreparePackageResult failed;
                  failed.diagnostics.push_back(
                      operationFailure("skin folder preparation failed"));
                  result.emplace(std::move(failed));
                } catch (...) {
                  PreparePackageResult failed;
                  failed.diagnostics.push_back(
                      operationFailure("unknown prepare error"));
                  result.emplace(std::move(failed));
                }
              }
            },
            request->payload);
      } catch (...) {
        if (request->ticket != 0) {
          PreparePackageResult failed;
          failed.diagnostics.push_back(
              operationFailure("skin package operation failed"));
          result.emplace(std::move(failed));
        }
      }
      std::visit([](auto &operation) { operation.cleanup.run(); },
                 request->payload);

      if (request->ticket == 0) {
        continue;
      }
      try {
        std::scoped_lock lock(mutex);
        outstanding.erase(request->ticket);
        if (!request->detached.load(std::memory_order_acquire) && result) {
          completions.emplace(
              request->ticket,
              SkinPackageOperationCompletion{.ticket = request->ticket,
                                             .payload = std::move(*result)});
        }
      } catch (...) {
        request->detached.store(true, std::memory_order_release);
      }
    }
  }

  std::optional<SkinPackageOperationCompletion> poll(std::uint64_t ticket) {
    std::scoped_lock lock(mutex);
    const auto found = completions.find(ticket);
    if (found == completions.end()) {
      return std::nullopt;
    }
    auto completion = std::move(found->second);
    completions.erase(found);
    return completion;
  }

  void cancelAndDetach(std::uint64_t ticket) noexcept {
    std::scoped_lock lock(mutex);
    const auto found = outstanding.find(ticket);
    if (found != outstanding.end()) {
      found->second->detached.store(true, std::memory_order_release);
      found->second->stop.request_stop();
    }
    completions.erase(ticket);
  }

  void shutdown() noexcept {
    bool shouldJoin = false;
    {
      std::scoped_lock lock(mutex);
      if (closing) {
        shouldJoin = worker.joinable();
      } else {
        closing = true;
        for (auto &[ticket, request] : outstanding) {
          (void)ticket;
          request->stop.request_stop();
        }
        shouldJoin = worker.joinable();
      }
    }
    workAvailable.notify_all();
    if (shouldJoin) {
      std::call_once(joined, [this] {
        try {
          if (worker.joinable()) {
            worker.join();
          }
        } catch (...) {
        }
      });
    }
  }
};

SkinPackageOperationService::SkinPackageOperationService(
    SkinPackageStore &store, SkinEntryValidator &validator)
    : impl_(std::make_unique<Impl>(store, validator)) {}

SkinPackageOperationService::~SkinPackageOperationService() { shutdown(); }

SkinPackageOperationHandle
SkinPackageOperationService::submitPrepareArchive(std::filesystem::path zip,
                                                  SkinPackageId package,
                                                  SkinDeferredCleanup cleanup) {
  return impl_->enqueue(Impl::RequestPayload(
      Impl::PrepareArchiveRequest{.zip = std::move(zip),
                                  .package = std::move(package),
                                  .cleanup = std::move(cleanup)}));
}

SkinPackageOperationHandle
SkinPackageOperationService::submitPrepareFolder(std::filesystem::path folder,
                                                 SkinPackageId package,
                                                 SkinDeferredCleanup cleanup) {
  return impl_->enqueue(Impl::RequestPayload(
      Impl::PrepareFolderRequest{.folder = std::move(folder),
                                 .package = std::move(package),
                                 .cleanup = std::move(cleanup)}));
}

std::shared_ptr<const SkinPackageCatalogSnapshot>
SkinPackageOperationService::catalogSnapshot() const noexcept {
  return impl_->store.catalogSnapshot();
}

std::optional<SkinPackageOperationCompletion>
SkinPackageOperationService::poll(std::uint64_t ticket) {
  return impl_->poll(ticket);
}

void SkinPackageOperationService::cancelAndDetach(
    std::uint64_t ticket) noexcept {
  impl_->cancelAndDetach(ticket);
}

void SkinPackageOperationService::discardPrepared(PreparedPackage prepared,
                                                  SkinDeferredCleanup cleanup) {
  impl_->enqueueDiscard(std::move(prepared), std::move(cleanup));
}

void SkinPackageOperationService::shutdown() noexcept {
  if (impl_) {
    impl_->shutdown();
  }
}

} // namespace skin
