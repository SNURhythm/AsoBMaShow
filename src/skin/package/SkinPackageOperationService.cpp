#include "SkinPackageOperationService.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
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

PreparePackageResult failedPreparation(std::string_view message) noexcept {
  PreparePackageResult result;
  try {
    result.diagnostics.push_back({.code = "skin.package.operation.failed",
                                  .message = std::string(message),
                                  .severity = DiagnosticSeverity::Error});
  } catch (...) {
    // The empty typed failure remains terminal and allocation-free.
  }
  return result;
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
  try {
    std::scoped_lock lock(state_->mutex);
    state_->progress = progress;
  } catch (...) {
  }
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

  static_assert(std::is_nothrow_move_constructible_v<RequestPayload>);
  static_assert(
      std::is_nothrow_move_constructible_v<SkinPackageOperationPayload>);

  enum class SlotState : std::uint8_t {
    Free,
    QueuedOperation,
    RunningOperation,
    Completing,
    Completed,
    QueuedDisposal,
    RunningDisposal,
  };

  struct Slot {
    SlotState state = SlotState::Free;
    std::uint64_t ticket = 0;
    std::shared_ptr<SkinPackageProgressMailbox> mailbox;
    std::optional<std::stop_source> stop;
    bool detached = false;
    std::optional<RequestPayload> request;
    std::optional<SkinPackageOperationPayload> result;
  };

  SkinPackageStore &store;
  SkinEntryValidator &validator;
#if defined(ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING)
  std::shared_ptr<const SkinPackageOperationTestObserver> observer;
#endif
  std::mutex mutex;
  std::condition_variable workAvailable;
  bool closing = false;
  std::array<Slot, maxRetainedOperations> slots;
  std::array<std::size_t, maxRetainedOperations> pending{};
  std::size_t pendingHead = 0;
  std::size_t pendingCount = 0;
  std::jthread worker;
  std::once_flag joined;

  Impl(SkinPackageStore &operationStore, SkinEntryValidator &operationValidator
#if defined(ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING)
       ,
       std::shared_ptr<const SkinPackageOperationTestObserver> testObserver
#endif
       )
      : store(operationStore), validator(operationValidator),
#if defined(ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING)
        observer(std::move(testObserver)),
#endif
        worker([this] { run(); }) {
  }

  ~Impl() { shutdown(); }

  static SkinDeferredCleanup takeCleanup(RequestPayload &payload) noexcept {
    return std::visit(
        [](auto &operation) { return std::move(operation.cleanup); }, payload);
  }

  static SkinPackageOperationHandle reject(RequestPayload &payload) noexcept {
    SkinPackageOperationHandle handle;
    handle.rejectedCleanup.emplace(takeCleanup(payload));
    return handle;
  }

  std::optional<std::size_t> findFreeSlotLocked() const noexcept {
    for (std::size_t index = 0; index < slots.size(); ++index) {
      if (slots[index].state == SlotState::Free) {
        return index;
      }
    }
    return std::nullopt;
  }

  void pushPendingLocked(std::size_t index) noexcept {
    pending[(pendingHead + pendingCount) % pending.size()] = index;
    ++pendingCount;
  }

  std::size_t popPendingLocked() noexcept {
    const std::size_t index = pending[pendingHead];
    pendingHead = (pendingHead + 1) % pending.size();
    --pendingCount;
    return index;
  }

  SkinPackageOperationHandle enqueue(RequestPayload payload) noexcept {
    std::shared_ptr<SkinPackageProgressMailbox> mailbox;
    std::optional<std::stop_source> operationStop;
    try {
      mailbox = std::shared_ptr<SkinPackageProgressMailbox>(
          new SkinPackageProgressMailbox(
              std::make_shared<SkinPackageProgressMailbox::State>()));
      operationStop.emplace();
    } catch (...) {
      return reject(payload);
    }

    std::uint64_t ticket = 0;
    try {
      std::scoped_lock lock(mutex);
      const auto freeSlot = findFreeSlotLocked();
      if (closing || !freeSlot || (ticket = allocateOperationTicket()) == 0) {
        return reject(payload);
      }
      Slot &slot = slots[*freeSlot];
      slot.ticket = ticket;
      slot.mailbox = mailbox;
      slot.stop.emplace(std::move(*operationStop));
      slot.detached = false;
      slot.request.emplace(std::move(payload));
      slot.result.reset();
      slot.state = SlotState::QueuedOperation;
      // Each active slot has at most one queue record, so a free slot proves
      // fixed-ring capacity without an allocation or failure branch.
      pushPendingLocked(*freeSlot);
    } catch (...) {
      return reject(payload);
    }
    workAvailable.notify_one();
    return {.ticket = ticket, .progress = std::move(mailbox)};
  }

  std::optional<RejectedPreparedDisposal>
  enqueueDiscard(DiscardPreparedRequest request) noexcept {
    std::optional<std::stop_source> operationStop;
    try {
      operationStop.emplace();
      std::scoped_lock lock(mutex);
      const auto freeSlot = findFreeSlotLocked();
      if (closing || !freeSlot) {
        return RejectedPreparedDisposal{.prepared = std::move(request.prepared),
                                        .cleanup = std::move(request.cleanup)};
      }
      Slot &slot = slots[*freeSlot];
      slot.ticket = 0;
      slot.mailbox.reset();
      slot.stop.emplace(std::move(*operationStop));
      slot.detached = false;
      slot.request.emplace(RequestPayload(std::move(request)));
      slot.result.reset();
      slot.state = SlotState::QueuedDisposal;
      pushPendingLocked(*freeSlot);
    } catch (...) {
      return RejectedPreparedDisposal{.prepared = std::move(request.prepared),
                                      .cleanup = std::move(request.cleanup)};
    }
    workAvailable.notify_one();
    return std::nullopt;
  }

  void takeSlotForDisposalLocked(
      std::size_t index, std::optional<RequestPayload> &releasedRequest,
      std::optional<SkinPackageOperationPayload> &releasedResult,
      std::shared_ptr<SkinPackageProgressMailbox> &releasedMailbox) noexcept {
    Slot &slot = slots[index];
    if (slot.request) {
      releasedRequest.emplace(std::move(*slot.request));
      slot.request.reset();
    }
    if (slot.result) {
      releasedResult.emplace(std::move(*slot.result));
      slot.result.reset();
    }
    releasedMailbox = std::move(slot.mailbox);
    slot.state = SlotState::RunningDisposal;
  }

  void finishSlotDisposalLocked(std::size_t index) noexcept {
    Slot &slot = slots[index];
    slot.stop.reset();
    slot.detached = false;
    slot.ticket = 0;
    slot.state = SlotState::Free;
  }

  std::optional<SkinPackageOperationPayload>
  executeOperation(Slot &slot) noexcept {
    std::optional<SkinPackageOperationPayload> result;
    try {
      std::visit(
          [&](auto &operation) {
            using Operation = std::decay_t<decltype(operation)>;
            if constexpr (std::is_same_v<Operation, PrepareArchiveRequest>) {
              try {
                result.emplace(store.prepareArchive(
                    operation.zip, operation.package, slot.stop->get_token(),
                    [mailbox = slot.mailbox](const SkinProgress &progress) {
                      mailbox->publish(progress);
                    }));
              } catch (...) {
                result.emplace(
                    failedPreparation("skin archive preparation failed"));
              }
            } else if constexpr (std::is_same_v<Operation,
                                                PrepareFolderRequest>) {
              try {
                result.emplace(store.prepareFolder(
                    operation.folder, operation.package, slot.stop->get_token(),
                    [mailbox = slot.mailbox](const SkinProgress &progress) {
                      mailbox->publish(progress);
                    }));
              } catch (...) {
                result.emplace(
                    failedPreparation("skin folder preparation failed"));
              }
            }
          },
          *slot.request);
    } catch (...) {
      if (slot.ticket != 0) {
        result.emplace(failedPreparation("skin package operation failed"));
      }
    }
    return result;
  }

  void run() noexcept {
    for (;;) {
      std::size_t index = 0;
      std::uint64_t disposalTicket = 0;
      bool disposal = false;
      std::optional<RequestPayload> releasedRequest;
      std::optional<SkinPackageOperationPayload> releasedResult;
      std::shared_ptr<SkinPackageProgressMailbox> releasedMailbox;
      try {
        std::unique_lock lock(mutex);
        workAvailable.wait(lock,
                           [this] { return closing || pendingCount != 0; });
        if (pendingCount == 0) {
          return;
        }
        index = popPendingLocked();
        Slot &slot = slots[index];
        disposal = slot.state == SlotState::QueuedDisposal;
        if (disposal) {
          disposalTicket = slot.ticket;
          slot.state = SlotState::RunningDisposal;
          takeSlotForDisposalLocked(index, releasedRequest, releasedResult,
                                    releasedMailbox);
        } else {
          slot.state = SlotState::RunningOperation;
        }
      } catch (...) {
        continue;
      }
      if (disposal) {
        if (releasedRequest) {
          std::visit([](auto &operation) { operation.cleanup.run(); },
                     *releasedRequest);
        }
#if defined(ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING)
        if (observer && disposalTicket != 0) {
          observer->disposing(disposalTicket);
        }
#endif
        releasedRequest.reset();
        releasedResult.reset();
        releasedMailbox.reset();
        try {
          std::scoped_lock lock(mutex);
          finishSlotDisposalLocked(index);
        } catch (...) {
          return;
        }
        continue;
      }

      Slot &slot = slots[index];
      auto result = executeOperation(slot);
      try {
        std::scoped_lock lock(mutex);
        if (!slot.detached && slot.ticket != 0) {
          slot.result.emplace(std::move(*result));
          slot.state = SlotState::Completing;
        }
      } catch (...) {
        // Slot/result moves are statically no-throw; a mutex failure leaves
        // the worker as the only owner and ends this process-level service.
        return;
      }
      std::visit([](auto &operation) { operation.cleanup.run(); },
                 *slot.request);
      bool completed = false;
      std::uint64_t completedTicket = 0;
      bool workerDisposal = false;
      try {
        std::scoped_lock lock(mutex);
        if (slot.detached || slot.ticket == 0) {
          takeSlotForDisposalLocked(index, releasedRequest, releasedResult,
                                    releasedMailbox);
          workerDisposal = true;
        } else {
          if (slot.request) {
            releasedRequest.emplace(std::move(*slot.request));
            slot.request.reset();
          }
          slot.state = SlotState::Completed;
          completed = true;
          completedTicket = slot.ticket;
        }
      } catch (...) {
        return;
      }
#if defined(ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING)
      if (completed && observer) {
        observer->completed(completedTicket);
      }
#endif
      if (workerDisposal) {
        releasedRequest.reset();
        releasedResult.reset();
        releasedMailbox.reset();
        try {
          std::scoped_lock lock(mutex);
          finishSlotDisposalLocked(index);
        } catch (...) {
          return;
        }
      }
    }
  }

  std::optional<SkinPackageOperationCompletion>
  poll(std::uint64_t ticket) noexcept {
    if (ticket == 0) {
      return std::nullopt;
    }
    try {
      std::scoped_lock lock(mutex);
      for (std::size_t index = 0; index < slots.size(); ++index) {
        Slot &slot = slots[index];
        if (slot.ticket != ticket || slot.state != SlotState::Completed ||
            !slot.result) {
          continue;
        }
        SkinPackageOperationCompletion completion{
            .ticket = ticket, .payload = std::move(*slot.result)};
        slot.result.reset();
        slot.mailbox.reset();
        slot.stop.reset();
        slot.detached = false;
        slot.ticket = 0;
        slot.state = SlotState::Free;
        return completion;
      }
    } catch (...) {
    }
    return std::nullopt;
  }

  void cancelAndDetach(std::uint64_t ticket) noexcept {
    if (ticket == 0) {
      return;
    }
    bool notify = false;
    try {
      std::scoped_lock lock(mutex);
      for (std::size_t index = 0; index < slots.size(); ++index) {
        Slot &slot = slots[index];
        if (slot.ticket != ticket || slot.state == SlotState::Free) {
          continue;
        }
        slot.detached = true;
        if (slot.stop) {
          slot.stop->request_stop();
        }
        if (slot.state == SlotState::Completed) {
          slot.state = SlotState::QueuedDisposal;
          pushPendingLocked(index);
          notify = true;
        }
        break;
      }
    } catch (...) {
    }
    if (notify) {
      workAvailable.notify_one();
    }
  }

  void shutdown() noexcept {
    bool shouldJoin = false;
    try {
      std::scoped_lock lock(mutex);
      if (!closing) {
        closing = true;
        for (std::size_t index = 0; index < slots.size(); ++index) {
          Slot &slot = slots[index];
          switch (slot.state) {
          case SlotState::QueuedOperation:
          case SlotState::RunningOperation:
          case SlotState::Completing:
            slot.detached = true;
            if (slot.stop) {
              slot.stop->request_stop();
            }
            break;
          case SlotState::Completed:
            slot.detached = true;
            slot.state = SlotState::QueuedDisposal;
            pushPendingLocked(index);
            break;
          default:
            break;
          }
        }
      }
      shouldJoin = worker.joinable();
    } catch (...) {
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
    SkinPackageStore &store, SkinEntryValidator &validator
#if defined(ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING)
    ,
    std::shared_ptr<const SkinPackageOperationTestObserver> observer
#endif
    )
    : impl_(std::make_unique<Impl>(store, validator
#if defined(ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING)
                                   ,
                                   std::move(observer)
#endif
                                       )) {
}

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

std::optional<RejectedPreparedDisposal>
SkinPackageOperationService::discardPrepared(PreparedPackage prepared,
                                             SkinDeferredCleanup cleanup) {
  return impl_->enqueueDiscard(Impl::DiscardPreparedRequest{
      .prepared = std::move(prepared), .cleanup = std::move(cleanup)});
}

void SkinPackageOperationService::shutdown() noexcept {
  if (impl_) {
    impl_->shutdown();
  }
}

} // namespace skin
