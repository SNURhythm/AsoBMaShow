#include "../targets.h"

#if TARGET_OS_LINUX && !TARGET_OS_ANDROID
#include "DesktopRealtimeKeyboardBackend.h"
#include "NativeKeyboardInputState.h"
#include "InputTimestamp.h"
#include "../perf/LatencyTelemetry.h"
#include <SDL2/SDL_log.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_video.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#if defined(ASOBMASHOW_HAVE_X11) && defined(SDL_VIDEO_DRIVER_X11)
#include <X11/Xlib.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <array>
#include <filesystem>
#include <future>
#include <vector>
#include "../../SDL/src/events/scancodes_linux.h"
#endif

namespace {
std::uint64_t steadyMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

class LinuxRealtimeKeyboardBackend final : public IInputBackend {
public:
  LinuxRealtimeKeyboardBackend(input::InputBackendSink sink,
                                std::shared_ptr<RealtimeControllerDeviceMap> map)
      : IInputBackend(std::move(sink)), map_(std::move(map)),
        keys_([this](auto event) { publishInput(std::move(event)); }) {}
  ~LinuxRealtimeKeyboardBackend() override { stop(); }

  bool start(std::string &errorMessage) override {
#if defined(ASOBMASHOW_HAVE_X11) && defined(SDL_VIDEO_DRIVER_X11)
    // SDL performs XInitThreads before opening its own X11 connection. Only
    // this worker uses our separate connection; never touch SDL's Display.
    const char *driver = SDL_GetCurrentVideoDriver();
    const char *wayland = std::getenv("WAYLAND_DISPLAY");
    if (driver == nullptr || std::string_view(driver) != "x11" ||
        (wayland != nullptr && *wayland != '\0') || !captureWindow()) {
      errorMessage = "Native keyboard requires an X11 window; using SDL";
      return false;
    }
    if (!openDevices(errorMessage)) { closeDevices(); return false; }
    std::promise<bool> ready;
    auto started = ready.get_future();
    running_.store(true);
    thread_ = std::thread([this, ready = std::move(ready)]() mutable {
      Display *display = XOpenDisplay(nullptr);
      if (display == nullptr) { ready.set_value(false); return; }
      // evdev on XWayland cannot establish the active Wayland surface safely.
      int opcode = 0, event = 0, error = 0;
      if (XQueryExtension(display, "XWAYLAND", &opcode, &event, &error)) {
        XCloseDisplay(display);
        ready.set_value(false);
        return;
      }
      timespec monotonic{};
      const auto before = steadyMicros();
      clock_gettime(CLOCK_MONOTONIC, &monotonic);
      const auto after = steadyMicros();
      const input::TimestampEpochMapping mapping{
          .sourceEpochMicros = static_cast<std::uint64_t>(monotonic.tv_sec) * 1000000 +
                               static_cast<std::uint64_t>(monotonic.tv_nsec) / 1000,
          .steadyEpochMicros = static_cast<std::int64_t>(before + (after - before) / 2)};
      available_.store(true, std::memory_order_release);
      ready.set_value(true);
      run(display, mapping);
      XCloseDisplay(display);
    });
    if (!started.get()) {
      errorMessage = "Native keyboard focus connection unavailable; using SDL";
      stop();
      return false;
    }
    map_->setKeyboardRealtimeAvailable(available_.load(std::memory_order_acquire));
    return true;
#else
    errorMessage = "Native keyboard built without optional X11 support; using SDL";
    return false;
#endif
  }

  void stop() override {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    const std::lock_guard lock(lifecycleMutex_);
    available_.store(false, std::memory_order_release);
    keys_.setEnabled(false, steadyMicros());
    map_->setKeyboardRealtimeAvailable(false);
#if defined(ASOBMASHOW_HAVE_X11) && defined(SDL_VIDEO_DRIVER_X11)
    closeDevices();
#endif
  }

  void pump() override {}
  void setRealtimeInputClaimed(input::DeviceClass deviceClass, bool claimed) override {
    if (deviceClass != input::DeviceClass::Keyboard) return;
    const std::lock_guard lock(lifecycleMutex_);
    claimed_ = claimed;
    claimTimestampMicros_ = steadyMicros();
    // Do not inherit keys held before gameplay obtained ownership.
    keys_.setEnabled(false, steadyMicros());
  }

  void handleSdlEvent(const SDL_Event &event) override {
#if defined(ASOBMASHOW_HAVE_X11) && defined(SDL_VIDEO_DRIVER_X11)
    if (event.type == SDL_WINDOWEVENT) {
      if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) captureWindow();
      if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        const std::lock_guard lock(lifecycleMutex_);
        keys_.setEnabled(false, steadyMicros());
      }
    }
#else
    (void)event;
#endif
  }

private:
#if defined(ASOBMASHOW_HAVE_X11) && defined(SDL_VIDEO_DRIVER_X11)
  bool captureWindow() {
    auto *window = SDL_GetKeyboardFocus(); // main-thread lifecycle only
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (window == nullptr || !SDL_GetWindowWMInfo(window, &info) ||
        info.subsystem != SDL_SYSWM_X11) return false;
    window_.store(info.info.x11.window, std::memory_order_release);
    return true;
  }

  bool openDevices(std::string &error) {
    monitor_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (monitor_ < 0 || inotify_add_watch(monitor_, "/dev/input",
        IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO) < 0) {
      error = "Native keyboard device monitoring unavailable; using SDL";
      return false;
    }
    std::error_code ec;
    std::filesystem::directory_iterator entries("/dev/input", ec);
    if (ec) { error = "Native keyboard devices unavailable; using SDL"; return false; }
    for (const auto &entry : entries) {
      if (!entry.path().filename().string().starts_with("event")) continue;
      const int fd = open(entry.path().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      // Conservatively require all event nodes to be readable. Otherwise an
      // inaccessible second keyboard could be lost when SDL duplicates stop.
      if (fd < 0) { error = "Native keyboard devices lack read access; using SDL"; return false; }
      std::array<unsigned long, (KEY_MAX + 1 + sizeof(unsigned long) * 8 - 1) /
                                    (sizeof(unsigned long) * 8)> bits{};
      const auto key = [&](int code) {
        return (bits[code / (sizeof(unsigned long) * 8)] &
                (1UL << (code % (sizeof(unsigned long) * 8)))) != 0;
      };
      if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits.data()) < 0) {
        close(fd); error = "Native keyboard capabilities unavailable; using SDL"; return false;
      }
      // Include keypads, macro pads and standard keyboards, but not mice.
      bool keyboard = false;
      for (std::size_t code = KEY_ESC; code < std::size(linux_scancode_table) && code <= KEY_MAX; ++code) {
        keyboard = keyboard || (linux_scancode_table[code] != SDL_SCANCODE_UNKNOWN && key(code));
      }
      if (!keyboard) { close(fd); continue; }
      int clock = CLOCK_MONOTONIC;
      if (fds_.size() >= NativeKeyboardInputState::kMaxDevices ||
          ioctl(fd, EVIOCSCLOCKID, &clock) < 0) {
        close(fd); error = "Native keyboard monotonic timestamps unavailable; using SDL"; return false;
      }
      fds_.push_back(fd);
    }
    if (fds_.empty()) { error = "No readable native keyboards; using SDL"; return false; }
    return true;
  }

  void closeDevices() {
    for (int fd : fds_) close(fd);
    fds_.clear();
    if (monitor_ >= 0) { close(monitor_); monitor_ = -1; }
  }

  void fallback() {
    const std::lock_guard lock(lifecycleMutex_);
    keys_.setEnabled(false, steadyMicros());
    available_.store(false, std::memory_order_release);
    map_->requestKeyboardRealtimeFallback();
    SDL_Log("Native keyboard device stream changed; continuing with SDL input");
  }

  void run(Display *display, input::TimestampEpochMapping mapping) {
    std::vector<pollfd> polls;
    for (int fd : fds_) polls.push_back({fd, POLLIN, 0});
    polls.push_back({monitor_, POLLIN, 0});
    while (running_.load(std::memory_order_acquire)) {
      const int result = poll(polls.data(), polls.size(), 10);
      if (result < 0) { if (errno == EINTR) continue; fallback(); return; }
      if (polls.back().revents != 0) { fallback(); return; }
      Window focus = None;
      int revert = 0;
      XGetInputFocus(display, &focus, &revert);
      const bool focused = focus == window_.load(std::memory_order_acquire);
      const std::lock_guard lock(lifecycleMutex_);
      keys_.setEnabled(claimed_ && focused, claimTimestampMicros_);
      for (std::size_t device = 0; device < fds_.size(); ++device) {
        if ((polls[device].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          keys_.setEnabled(false, steadyMicros());
          available_.store(false, std::memory_order_release);
          map_->requestKeyboardRealtimeFallback();
          return;
        }
        input_event events[64];
        ssize_t bytes;
        while ((bytes = read(fds_[device], events, sizeof(events))) > 0) {
          for (std::size_t i = 0; i < static_cast<std::size_t>(bytes) / sizeof(input_event); ++i) {
            const auto &event = events[i];
            if (event.type == EV_SYN && event.code == SYN_DROPPED) {
              // Lost edges cannot be reconstructed with original timing.
              // Release ownership and let SDL's independently maintained state
              // continue, rather than inventing replacement key timestamps.
              keys_.setEnabled(false, steadyMicros());
              available_.store(false, std::memory_order_release);
              map_->requestKeyboardRealtimeFallback();
              return;
            }
            if (event.type != EV_KEY || event.value == 2 ||
                event.code >= std::size(linux_scancode_table)) continue;
            const auto nativeMicros = static_cast<std::uint64_t>(event.input_event_sec) * 1000000 +
                                      static_cast<std::uint64_t>(event.input_event_usec);
            const auto timestamp = mapping.toSteadyMicros(nativeMicros);
            if (timestamp < 0 || static_cast<std::uint64_t>(timestamp) < claimTimestampMicros_) continue;
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
            const auto receipt = perf::latency::nowMicros();
            if (claimed_ && focused && receipt >= timestamp) {
              perf::latency::record(perf::latency::Stage::InputDelivery, receipt - timestamp);
            }
#endif
            keys_.consume(device, linux_scancode_table[event.code], event.value != 0, timestamp);
          }
        }
        if (bytes < 0 && errno != EAGAIN && errno != EINTR) {
          keys_.setEnabled(false, steadyMicros());
          available_.store(false, std::memory_order_release);
          map_->requestKeyboardRealtimeFallback();
          return;
        }
      }
    }
  }

  std::atomic<Window> window_{None};
  std::vector<int> fds_;
  int monitor_ = -1;
#endif
  std::shared_ptr<RealtimeControllerDeviceMap> map_;
  NativeKeyboardInputState keys_;
  std::mutex lifecycleMutex_;
  bool claimed_ = false;
  std::uint64_t claimTimestampMicros_ = 0;
  std::atomic_bool running_{false}, available_{false};
  std::thread thread_;
};
}

std::unique_ptr<IInputBackend> makeDesktopRealtimeKeyboardBackend(
    input::InputBackendSink sink,
    std::shared_ptr<RealtimeControllerDeviceMap> deviceMap) {
  return std::make_unique<LinuxRealtimeKeyboardBackend>(std::move(sink), std::move(deviceMap));
}
#endif
