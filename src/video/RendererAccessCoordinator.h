#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>

namespace display {
class RendererAccessCoordinator {
public:
  class DisplayReservation {
  public:
    DisplayReservation(DisplayReservation &&) noexcept = default;
    DisplayReservation &operator=(DisplayReservation &&) noexcept = default;
    DisplayReservation(const DisplayReservation &) = delete;
    DisplayReservation &operator=(const DisplayReservation &) = delete;
    bool ownsLock() const;

  private:
    friend class RendererAccessCoordinator;
    explicit DisplayReservation(std::unique_lock<std::mutex> lockValue);
    std::unique_lock<std::mutex> lock;
  };

  class ExportReservation {
  public:
    ExportReservation(ExportReservation &&other) noexcept;
    ExportReservation &operator=(ExportReservation &&other) noexcept = delete;
    ExportReservation(const ExportReservation &) = delete;
    ExportReservation &operator=(const ExportReservation &) = delete;
    ~ExportReservation();

    void unlockForUiFrame();
    void relockAfterUiFrame();
    void release();
    bool ownsLock() const;

  private:
    friend class RendererAccessCoordinator;
    ExportReservation(std::mutex &rendererMutex,
                      std::atomic<bool> &exportActive,
                      std::size_t &activeExports);
    std::unique_lock<std::mutex> lock;
    std::atomic<bool> *exportActive = nullptr;
    std::size_t *activeExports = nullptr;
    bool released = false;
  };

  RendererAccessCoordinator(std::mutex &rendererMutex,
                            std::atomic<bool> &exportActive);

  std::optional<DisplayReservation>
  tryAcquireDisplay(std::string &errorMessage);
  ExportReservation acquireExport();

private:
  std::mutex &rendererMutex;
  std::atomic<bool> &exportActive;
  std::size_t activeExports = 0;
};
} // namespace display
