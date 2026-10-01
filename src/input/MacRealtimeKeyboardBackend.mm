#include "../targets.h"

#if TARGET_OS_OSX
#include "DesktopRealtimeKeyboardBackend.h"
#include "NativeKeyboardInputState.h"
#include "AppleInputTimestamp.h"
#include "../perf/LatencyTelemetry.h"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <SDL2/SDL_log.h>
#include "../../SDL/src/events/scancodes_darwin.h"

#include <atomic>
#include <future>
#include <thread>
#include <unistd.h>

namespace {

class MacRealtimeKeyboardBackend final : public IInputBackend {
public:
  MacRealtimeKeyboardBackend(input::InputBackendSink sink,
                              std::shared_ptr<RealtimeControllerDeviceMap> map)
      : IInputBackend(std::move(sink)), map_(std::move(map)),
        keys_([this](auto event) { publishInput(std::move(event)); }) {}

  ~MacRealtimeKeyboardBackend() override { stop(); }

  bool start(std::string &errorMessage) override {
    // Never trigger a privacy prompt. Ordinary SDL input remains available.
    if (@available(macOS 10.15, *)) {
      if (!CGPreflightListenEventAccess()) {
        errorMessage = "Native keyboard unavailable: existing Input Monitoring permission required; using SDL";
        return false;
      }
    } else {
      errorMessage = "Native keyboard permission preflight unavailable; using SDL";
      return false;
    }
    std::promise<bool> ready;
    auto started = ready.get_future();
    running_.store(true);
    thread_ = std::thread([this, ready = std::move(ready)]() mutable {
      timestampSession_.reanchor();
      tap_ = CGEventTapCreateForPid(
          getpid(), kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
          CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
              CGEventMaskBit(kCGEventFlagsChanged), &eventTap, this);
      if (tap_ == nullptr) { ready.set_value(false); return; }
      source_ = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap_, 0);
      if (source_ == nullptr) { ready.set_value(false); return; }
      runLoop_ = CFRunLoopGetCurrent();
      CFRetain(runLoop_);
      CFRunLoopAddSource(runLoop_, source_, kCFRunLoopDefaultMode);
      available_.store(true, std::memory_order_release);
      ready.set_value(true);
      while (running_.load(std::memory_order_acquire)) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
      }
      CFRunLoopRemoveSource(runLoop_, source_, kCFRunLoopDefaultMode);
    });
    if (!started.get()) {
      errorMessage = "Native per-process keyboard tap unavailable; using SDL";
      stop();
      return false;
    }
    map_->setKeyboardRealtimeAvailable(available_.load(std::memory_order_acquire));
    return true;
  }

  void stop() override {
    map_->setKeyboardRealtimeAvailable(false);
    available_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    if (runLoop_ != nullptr) CFRunLoopWakeUp(runLoop_);
    if (thread_.joinable()) thread_.join();
    keys_.setClaimed(false, input::apple::steadyNowMicros());
    if (source_ != nullptr) { CFRelease(source_); source_ = nullptr; }
    if (tap_ != nullptr) { CFMachPortInvalidate(tap_); CFRelease(tap_); tap_ = nullptr; }
    if (runLoop_ != nullptr) { CFRelease(runLoop_); runLoop_ = nullptr; }
  }

  void pump() override {}

  void setRealtimeInputClaimed(input::DeviceClass deviceClass, bool claimed) override {
    if (deviceClass != input::DeviceClass::Keyboard) return;
    const std::lock_guard lock(lifecycleMutex_);
    claimed_.store(claimed, std::memory_order_release);
    claimTimestampMicros_ = input::apple::steadyNowMicros();
    if (claimed && !available_.load(std::memory_order_acquire) &&
        map_->keyboardRealtimeAvailable()) {
      // Failure raced the registry's pre-claim handoff. The lifecycle lock
      // guarantees its fallback request is ready before this new owner starts.
      const auto timestamp = static_cast<std::uint64_t>(claimTimestampMicros_);
      publishInterruption({input::DeviceClass::Keyboard, timestamp, false});
      publishInterruption({input::DeviceClass::Keyboard, timestamp, true});
    }
    keys_.setClaimed(claimed && available_.load(std::memory_order_acquire),
                     claimTimestampMicros_);
  }

  void handleSdlEvent(const SDL_Event &event) override {
    if (event.type == SDL_WINDOWEVENT &&
        event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
      const std::lock_guard lock(lifecycleMutex_);
      keys_.setFocused(false, input::apple::steadyNowMicros());
    }
  }

private:
  static CGEventRef eventTap(CGEventTapProxy, CGEventType type, CGEventRef event,
                             void *context) {
    auto &self = *static_cast<MacRealtimeKeyboardBackend *>(context);
    const std::lock_guard lock(self.lifecycleMutex_);
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
      if (!self.available_.exchange(false, std::memory_order_acq_rel)) return event;
      const auto timestamp = static_cast<std::uint64_t>(input::apple::steadyNowMicros());
      const bool interrupt = self.claimed_.load(std::memory_order_acquire);
      if (interrupt) self.publishInterruption({input::DeviceClass::Keyboard, timestamp, false});
      self.keys_.setClaimed(false, timestamp);
      self.map_->requestKeyboardRealtimeFallback();
      if (interrupt) self.publishInterruption({input::DeviceClass::Keyboard, timestamp, true});
      SDL_Log("Native keyboard tap disabled; SDL fallback requested");
      return event;
    }
    if (!self.claimed_.load(std::memory_order_acquire) ||
        !self.available_.load(std::memory_order_acquire) || event == nullptr) return event;
    if (type != kCGEventKeyDown && type != kCGEventKeyUp &&
        type != kCGEventFlagsChanged) return event;
    if (type == kCGEventKeyDown &&
        CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0) return event;
    auto key = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    const auto keyboardType = CGEventGetIntegerValueField(event, kCGKeyboardEventKeyboardType);
    if ((key == 10 || key == 50) && KBGetLayoutType(keyboardType) == kKeyboardISO) key = 60 - key;
    if (key < 0 || static_cast<std::size_t>(key) >= std::size(darwin_scancode_table)) return event;
    const auto scancode = darwin_scancode_table[key];
    bool pressed = type == kCGEventKeyDown;
    if (type == kCGEventFlagsChanged) {
      CGEventFlags mask = 0;
      switch (scancode) {
      case SDL_SCANCODE_LSHIFT: mask = NX_DEVICELSHIFTKEYMASK; break;
      case SDL_SCANCODE_RSHIFT: mask = NX_DEVICERSHIFTKEYMASK; break;
      case SDL_SCANCODE_LCTRL: mask = NX_DEVICELCTLKEYMASK; break;
      case SDL_SCANCODE_RCTRL: mask = NX_DEVICERCTLKEYMASK; break;
      case SDL_SCANCODE_LALT: mask = NX_DEVICELALTKEYMASK; break;
      case SDL_SCANCODE_RALT: mask = NX_DEVICERALTKEYMASK; break;
      case SDL_SCANCODE_LGUI: mask = NX_DEVICELCMDKEYMASK; break;
      case SDL_SCANCODE_RGUI: mask = NX_DEVICERCMDKEYMASK; break;
      case SDL_SCANCODE_CAPSLOCK: mask = kCGEventFlagMaskAlphaShift; break;
      default: return event;
      }
      pressed = (CGEventGetFlags(event) & mask) != 0;
    }
    // CGEvent timestamps are nanoseconds in the Mach absolute-time epoch.
    const auto nativeTimestamp = CGEventGetTimestamp(event);
    const auto timestamp = nativeTimestamp != 0
        ? self.timestampSession_.toSteadyMicros(nativeTimestamp / 1000)
        : input::apple::steadyNowMicros();
    if (timestamp < self.claimTimestampMicros_) return event;
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
    const auto receipt = perf::latency::nowMicros();
    if (nativeTimestamp != 0 && receipt >= timestamp) {
      perf::latency::record(perf::latency::Stage::InputDelivery, receipt - timestamp);
    }
#endif
    // The OS routes this per-process tap only to our application, including
    // events delivered before SDL has pumped a focus-gained notification.
    self.keys_.setFocused(true, timestamp);
    self.keys_.consume(0, scancode, pressed, timestamp);
    return event;
  }

  std::shared_ptr<RealtimeControllerDeviceMap> map_;
  NativeKeyboardInputState keys_;
  std::mutex lifecycleMutex_;
  std::int64_t claimTimestampMicros_ = 0;
  input::apple::HostToSteadyTimestampSession timestampSession_;
  std::atomic_bool running_{false}, claimed_{false}, available_{false};
  std::thread thread_;
  CFMachPortRef tap_ = nullptr;
  CFRunLoopSourceRef source_ = nullptr;
  CFRunLoopRef runLoop_ = nullptr;
};
}

std::unique_ptr<IInputBackend> makeDesktopRealtimeKeyboardBackend(
    input::InputBackendSink sink,
    std::shared_ptr<RealtimeControllerDeviceMap> deviceMap) {
  return std::make_unique<MacRealtimeKeyboardBackend>(std::move(sink), std::move(deviceMap));
}
#endif
