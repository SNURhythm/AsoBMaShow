#include "WindowsRealtimeInputBackend.h"

#if defined(_WIN32)

#include "WindowsRealtimeInputMapping.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Xinput.h>
#include <avrt.h>
#include <Unknwn.h>
#include <rtworkq.h>

#include <SDL2/SDL_log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

constexpr std::uint64_t kActivePollsPerSecond = 1000;
constexpr std::uint64_t kIdlePollsPerSecond = 60;

class WindowsRealtimeInputBackend;

class RtwqCallback final : public IRtwqAsyncCallback {
public:
  using InvokeFunction = HRESULT (*)(void *, IRtwqAsyncResult *);

  RtwqCallback(void *context, InvokeFunction invoke)
      : context_(context), invoke_(invoke) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void **object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IRtwqAsyncCallback)) {
      *object = static_cast<IRtwqAsyncCallback *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const LONG remaining = InterlockedDecrement(&references_);
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  HRESULT STDMETHODCALLTYPE GetParameters(DWORD *, DWORD *) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE Invoke(IRtwqAsyncResult *result) override {
    return invoke_ == nullptr ? E_UNEXPECTED : invoke_(context_, result);
  }

private:
  ~RtwqCallback() = default;

  volatile LONG references_ = 1;
  void *context_ = nullptr;
  InvokeFunction invoke_ = nullptr;
};

class QpcTimeline {
public:
  bool initialize() noexcept {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter)) {
      return false;
    }
    frequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
    qpcOrigin_ = static_cast<std::uint64_t>(counter.QuadPart);
    steadyOriginMicros_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    return true;
  }

  [[nodiscard]] std::uint64_t counter() const noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
  }

  [[nodiscard]] std::uint64_t frequency() const noexcept {
    return frequency_;
  }

  [[nodiscard]] std::uint64_t micros(std::uint64_t qpc) const noexcept {
    const std::uint64_t elapsed = qpc >= qpcOrigin_ ? qpc - qpcOrigin_ : 0;
    const std::uint64_t seconds = elapsed / frequency_;
    const std::uint64_t remainder = elapsed % frequency_;
    return steadyOriginMicros_ + seconds * 1000000ULL +
           remainder * 1000000ULL / frequency_;
  }

  [[nodiscard]] std::uint64_t ticksForRate(
      std::uint64_t rate) const noexcept {
    return std::max<std::uint64_t>(1, frequency_ / rate);
  }

private:
  std::uint64_t frequency_ = 1;
  std::uint64_t qpcOrigin_ = 0;
  std::uint64_t steadyOriginMicros_ = 0;
};

using XInputGetStateFunction = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);

struct ControllerSlot {
  std::shared_ptr<const std::string> stableId;
  WindowsGameControllerState observed;
  WindowsGameControllerState published;
  DWORD packetNumber = 0;
  bool connected = false;
  bool hasObserved = false;
  bool publishing = false;
  std::uint64_t identityGeneration = 0;
};

class WindowsRealtimeInputBackend final : public IInputBackend {
public:
  WindowsRealtimeInputBackend(
      input::InputBackendSink sink,
      std::shared_ptr<RealtimeControllerDeviceMap> controllerMap)
      : IInputBackend(std::move(sink)),
        controllerMap_(std::move(controllerMap)) {}

  ~WindowsRealtimeInputBackend() override { stop(); }

  bool start(std::string &errorMessage) override {
    errorMessage.clear();
    if (started_) {
      return true;
    }
    if (!timeline_.initialize()) {
      errorMessage = "QueryPerformanceCounter is unavailable";
      return false;
    }
    if (!createEvents()) {
      errorMessage = "could not create Windows realtime input events";
      cleanup();
      return false;
    }

    HRESULT result = RtwqStartup();
    if (FAILED(result)) {
      errorMessage = "RtwqStartup failed";
      cleanup();
      return false;
    }
    rtwqStarted_ = true;
    result = RtwqAllocateWorkQueue(RTWQ_STANDARD_WORKQUEUE, &workQueueId_);
    if (FAILED(result)) {
      errorMessage = "RtwqAllocateWorkQueue failed";
      cleanup();
      return false;
    }
    workQueueAllocated_ = true;
    if (FAILED(RtwqSetLongRunning(workQueueId_, TRUE))) {
      SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                  "RTWorkQ did not accept the long-running input hint");
    }

    completionCallback_ = new RtwqCallback(
        this, &WindowsRealtimeInputBackend::completionCallbackInvoke);
    workerCallback_ = new RtwqCallback(
        this, &WindowsRealtimeInputBackend::workerCallbackInvoke);
    if (!registerMmcss()) {
      SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                  "RTWorkQ input queue could not join MMCSS Games");
    }

    result = RtwqCreateAsyncResult(nullptr, workerCallback_, nullptr,
                                   &workerResult_);
    if (FAILED(result)) {
      errorMessage = "RtwqCreateAsyncResult failed for input worker";
      cleanup();
      return false;
    }
    result = RtwqPutWorkItem(workQueueId_, 1, workerResult_);
    if (FAILED(result)) {
      errorMessage = "RtwqPutWorkItem failed for input worker";
      cleanup();
      return false;
    }
    workerSubmitted_ = true;
    if (WaitForSingleObject(workerReadyEvent_, INFINITE) != WAIT_OBJECT_0 ||
        FAILED(workerStartResult_.load(std::memory_order_acquire))) {
      errorMessage = "Windows low-level keyboard hook failed";
      cleanup();
      return false;
    }
    started_ = true;
    return true;
  }

  void stop() override {
    started_ = false;
    cleanup();
  }

  void pump() override {}

  void setRealtimeInputClaimed(input::DeviceClass deviceClass,
                               bool claimed) override {
    std::atomic_bool *target = nullptr;
    if (deviceClass == input::DeviceClass::Keyboard) {
      target = &keyboardClaimRequested_;
    } else if (deviceClass == input::DeviceClass::GameController) {
      target = &controllerClaimRequested_;
    } else {
      return;
    }
    if (target->exchange(claimed, std::memory_order_acq_rel) == claimed ||
        !workerSubmitted_) {
      return;
    }
    const std::uint64_t generation =
        requestedClaimGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    SetEvent(wakeEvent_);
    while (appliedClaimGeneration_.load(std::memory_order_acquire) <
           generation) {
      if (WaitForSingleObject(claimAppliedEvent_, INFINITE) != WAIT_OBJECT_0) {
        break;
      }
    }
  }

private:
  enum class CompletionOperation { None, RegisterMmcss, UnregisterMmcss };

  static HRESULT completionCallbackInvoke(void *context,
                                          IRtwqAsyncResult *result) {
    return static_cast<WindowsRealtimeInputBackend *>(context)
        ->finishCompletion(result);
  }

  static HRESULT workerCallbackInvoke(void *context, IRtwqAsyncResult *) {
    static_cast<WindowsRealtimeInputBackend *>(context)->runWorker();
    return S_OK;
  }

  static LRESULT CALLBACK keyboardHook(int code, WPARAM message,
                                       LPARAM data) {
    auto *owner = hookOwner_.load(std::memory_order_acquire);
    if (code == HC_ACTION && owner != nullptr && data != 0) {
      owner->acceptKeyboardHook(
          message, *reinterpret_cast<const KBDLLHOOKSTRUCT *>(data));
    }
    return CallNextHookEx(nullptr, code, message, data);
  }

  bool createEvents() {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    claimAppliedEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    completionEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    workerReadyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    workerDoneEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    timer_ = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (timer_ == nullptr) {
      timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    return stopEvent_ != nullptr && wakeEvent_ != nullptr &&
           claimAppliedEvent_ != nullptr && completionEvent_ != nullptr &&
           workerReadyEvent_ != nullptr && workerDoneEvent_ != nullptr &&
           timer_ != nullptr;
  }

  bool registerMmcss() {
    completionOperation_.store(CompletionOperation::RegisterMmcss,
                               std::memory_order_release);
    completionResult_.store(E_PENDING, std::memory_order_release);
    ResetEvent(completionEvent());
    const HRESULT result = RtwqBeginRegisterWorkQueueWithMMCSS(
        workQueueId_, L"Games", 0, AVRT_PRIORITY_HIGH, completionCallback_,
        nullptr);
    if (FAILED(result) ||
        WaitForSingleObject(completionEvent(), INFINITE) != WAIT_OBJECT_0 ||
        FAILED(completionResult_.load(std::memory_order_acquire))) {
      completionOperation_.store(CompletionOperation::None,
                                 std::memory_order_release);
      return false;
    }
    mmcssRegistered_ = true;
    return true;
  }

  void unregisterMmcss() {
    if (!mmcssRegistered_ || completionCallback_ == nullptr) {
      return;
    }
    completionOperation_.store(CompletionOperation::UnregisterMmcss,
                               std::memory_order_release);
    completionResult_.store(E_PENDING, std::memory_order_release);
    ResetEvent(completionEvent());
    const HRESULT result = RtwqBeginUnregisterWorkQueueWithMMCSS(
        workQueueId_, completionCallback_, nullptr);
    if (SUCCEEDED(result)) {
      (void)WaitForSingleObject(completionEvent(), INFINITE);
    }
    completionOperation_.store(CompletionOperation::None,
                               std::memory_order_release);
    mmcssRegistered_ = false;
  }

  HANDLE completionEvent() const noexcept { return completionEvent_; }

  HRESULT finishCompletion(IRtwqAsyncResult *result) {
    HRESULT completion = E_UNEXPECTED;
    switch (completionOperation_.load(std::memory_order_acquire)) {
    case CompletionOperation::RegisterMmcss:
      completion = RtwqEndRegisterWorkQueueWithMMCSS(result, &mmcssTaskId_);
      break;
    case CompletionOperation::UnregisterMmcss:
      completion = finishMmcssUnregistration(result);
      break;
    case CompletionOperation::None:
      break;
    }
    completionResult_.store(completion, std::memory_order_release);
    SetEvent(completionEvent());
    return S_OK;
  }

  static HRESULT finishMmcssUnregistration(IRtwqAsyncResult *result) {
    // Unlike registration, RTWorkQ exposes no EndUnregister function. The
    // callback result itself carries the asynchronous completion status.
    return result == nullptr ? E_POINTER : result->GetStatus();
  }

  void cleanup() {
    if (workerSubmitted_) {
      SetEvent(stopEvent_);
      SetEvent(wakeEvent_);
      (void)WaitForSingleObject(workerDoneEvent_, INFINITE);
      workerSubmitted_ = false;
    }
    if (workerResult_ != nullptr) {
      workerResult_->Release();
      workerResult_ = nullptr;
    }
    unregisterMmcss();
    if (workQueueAllocated_) {
      (void)RtwqSetLongRunning(workQueueId_, FALSE);
      (void)RtwqUnlockWorkQueue(workQueueId_);
      workQueueAllocated_ = false;
      workQueueId_ = 0;
    }
    if (workerCallback_ != nullptr) {
      workerCallback_->Release();
      workerCallback_ = nullptr;
    }
    if (completionCallback_ != nullptr) {
      completionCallback_->Release();
      completionCallback_ = nullptr;
    }
    if (rtwqStarted_) {
      (void)RtwqShutdown();
      rtwqStarted_ = false;
    }
    closeHandle(timer_);
    closeHandle(workerDoneEvent_);
    closeHandle(workerReadyEvent_);
    closeHandle(completionEvent_);
    closeHandle(claimAppliedEvent_);
    closeHandle(wakeEvent_);
    closeHandle(stopEvent_);
    unloadXInput();
    resetState();
  }

  static void closeHandle(HANDLE &handle) {
    if (handle != nullptr) {
      CloseHandle(handle);
      handle = nullptr;
    }
  }

  void resetState() {
    keyboardClaimRequested_.store(false);
    controllerClaimRequested_.store(false);
    requestedClaimGeneration_.store(0);
    appliedClaimGeneration_.store(0);
    keyboardClaimActive_ = false;
    controllerClaimActive_ = false;
    physicalKeys_.fill(false);
    publishedKeys_.fill(false);
    controllers_ = {};
    foregroundActive_ = false;
    workerStartResult_.store(E_PENDING);
    completionResult_.store(E_PENDING);
    completionOperation_.store(CompletionOperation::None);
    if (controllerMap_) {
      controllerMap_->setKeyboardRealtimeAvailable(false);
      controllerMap_->setControllerRealtimeAvailable(false);
    }
  }

  void loadXInput() {
    constexpr std::array libraries{L"xinput1_4.dll", L"xinput1_3.dll",
                                   L"xinput9_1_0.dll"};
    for (const auto *library : libraries) {
      xinputModule_ = LoadLibraryW(library);
      if (xinputModule_ == nullptr) {
        continue;
      }
      xinputGetState_ = reinterpret_cast<XInputGetStateFunction>(
          GetProcAddress(xinputModule_, "XInputGetState"));
      if (xinputGetState_ != nullptr) {
        return;
      }
      FreeLibrary(xinputModule_);
      xinputModule_ = nullptr;
    }
  }

  void unloadXInput() {
    xinputGetState_ = nullptr;
    if (xinputModule_ != nullptr) {
      FreeLibrary(xinputModule_);
      xinputModule_ = nullptr;
    }
  }

  void runWorker() {
    loadXInput();
    hookOwner_.store(this, std::memory_order_release);
    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &keyboardHook,
                                     GetModuleHandleW(nullptr), 0);
    if (keyboardHook_ == nullptr) {
      hookOwner_.store(nullptr, std::memory_order_release);
      const DWORD error = GetLastError();
      workerStartResult_.store(
          HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE
                                                    : error),
                               std::memory_order_release);
      SetEvent(workerReadyEvent_);
      SetEvent(workerDoneEvent_);
      return;
    }
    if (controllerMap_) {
      controllerMap_->setKeyboardRealtimeAvailable(true);
      controllerMap_->setControllerRealtimeAvailable(xinputGetState_ != nullptr);
    }
    workerStartResult_.store(S_OK, std::memory_order_release);
    SetEvent(workerReadyEvent_);

    bool wasRealtimeActive = false;
    std::uint64_t nextTick = timeline_.counter();
    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
      applyRequestedClaims();
      // Keyboard edges arrive through the low-level hook and need no busy
      // sampling. Only XInput ownership raises the timer to the active rate.
      const bool realtimeActive = controllerClaimActive_;
      if (realtimeActive != wasRealtimeActive) {
        nextTick = timeline_.counter();
        wasRealtimeActive = realtimeActive;
      }

      const std::uint64_t now = timeline_.counter();
      if (now >= nextTick) {
        poll(now);
        const std::uint64_t rate =
            realtimeActive ? kActivePollsPerSecond : kIdlePollsPerSecond;
        const std::uint64_t interval = timeline_.ticksForRate(rate);
        const std::uint64_t behind = now - nextTick;
        const std::uint64_t skipped = behind / interval + 1;
        nextTick += skipped * interval;
      }

      armTimer(nextTick, timeline_.counter());
      HANDLE handles[]{stopEvent_, wakeEvent_, timer_};
      const DWORD wait = MsgWaitForMultipleObjectsEx(
          static_cast<DWORD>(std::size(handles)), handles, INFINITE,
          QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      if (wait == WAIT_OBJECT_0) {
        break;
      }
      if (wait == WAIT_OBJECT_0 + std::size(handles)) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
      }
    }

    const std::uint64_t timestamp = timeline_.micros(timeline_.counter());
    releaseKeyboard(timestamp);
    releaseControllers(timestamp);
    UnhookWindowsHookEx(keyboardHook_);
    keyboardHook_ = nullptr;
    if (controllerMap_) {
      controllerMap_->setKeyboardRealtimeAvailable(false);
      controllerMap_->setControllerRealtimeAvailable(false);
    }
    hookOwner_.store(nullptr, std::memory_order_release);
    SetEvent(workerDoneEvent_);
  }

  void applyRequestedClaims() {
    const std::uint64_t requested =
        requestedClaimGeneration_.load(std::memory_order_acquire);
    if (requested ==
        appliedClaimGeneration_.load(std::memory_order_acquire)) {
      return;
    }
    const bool keyboard =
        keyboardClaimRequested_.load(std::memory_order_acquire);
    const bool controller =
        controllerClaimRequested_.load(std::memory_order_acquire);
    const std::uint64_t timestamp = timeline_.micros(timeline_.counter());
    if (keyboardClaimActive_ && !keyboard) {
      releaseKeyboard(timestamp);
    }
    if (controllerClaimActive_ && !controller) {
      releaseControllers(timestamp);
    }
    keyboardClaimActive_ = keyboard;
    controllerClaimActive_ = controller;
    updateForeground(timestamp);
    if (controllerClaimActive_) {
      pollControllers(timestamp);
    }
    if (appliedClaimGeneration_.exchange(requested,
                                         std::memory_order_acq_rel) !=
        requested) {
      SetEvent(claimAppliedEvent_);
    }
  }

  void armTimer(std::uint64_t deadline, std::uint64_t now) const {
    const std::uint64_t ticks = deadline > now ? deadline - now : 1;
    const std::uint64_t hundredNanos = std::max<std::uint64_t>(
        1, ticks * 10000000ULL / timeline_.frequency());
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(hundredNanos);
    (void)SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
  }

  void poll(std::uint64_t qpc) {
    const std::uint64_t timestamp = timeline_.micros(qpc);
    updateForeground(timestamp);
    pollControllers(timestamp);
  }

  bool currentProcessIsForeground() const {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
      return false;
    }
    DWORD processId = 0;
    (void)GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
  }

  void updateForeground(std::uint64_t timestamp) {
    const bool active = currentProcessIsForeground();
    if (foregroundActive_ && !active) {
      releaseKeyboard(timestamp);
      releaseControllers(timestamp);
    }
    foregroundActive_ = active;
  }

  void acceptKeyboardHook(WPARAM message, const KBDLLHOOKSTRUCT &event) {
    if ((event.flags & LLKHF_INJECTED) != 0) {
      return;
    }
    const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (!pressed && message != WM_KEYUP && message != WM_SYSKEYUP) {
      return;
    }
    const SDL_Scancode scancode = windowsRealtimeSdlScancode(
        event.vkCode, event.scanCode, (event.flags & LLKHF_EXTENDED) != 0);
    if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_NUM_SCANCODES) {
      return;
    }
    const std::size_t index = static_cast<std::size_t>(scancode);
    const bool previous = physicalKeys_[index];
    physicalKeys_[index] = pressed;
    if (previous == pressed) {
      return;
    }
    const std::uint64_t timestamp = timeline_.micros(timeline_.counter());
    updateForeground(timestamp);
    if (!keyboardClaimActive_ || !foregroundActive_) {
      return;
    }
    if (pressed) {
      publishKeyboard(scancode, true, timestamp);
      publishedKeys_[index] = true;
    } else if (publishedKeys_[index]) {
      publishKeyboard(scancode, false, timestamp);
      publishedKeys_[index] = false;
    }
  }

  void publishKeyboard(SDL_Scancode scancode, bool pressed,
                       std::uint64_t timestamp) {
    publishInput({.control = {.deviceId = "keyboard",
                              .deviceClass = input::DeviceClass::Keyboard,
                              .kind = input::ControlKind::Key,
                              .index = static_cast<int>(scancode)},
                  .rawValue = pressed ? 1.0 : 0.0,
                  .normalizedValue = pressed ? 1.0F : 0.0F,
                  .timestampMicros = timestamp});
  }

  void releaseKeyboard(std::uint64_t timestamp) {
    for (std::size_t index = 0; index < publishedKeys_.size(); ++index) {
      if (!publishedKeys_[index]) {
        continue;
      }
      publishKeyboard(static_cast<SDL_Scancode>(index), false, timestamp);
      publishedKeys_[index] = false;
    }
  }

  void pollControllers(std::uint64_t timestamp) {
    for (DWORD player = 0; player < controllers_.size(); ++player) {
      pollController(player, timestamp);
    }
  }

  void pollController(DWORD player, std::uint64_t timestamp) {
    auto &slot = controllers_[player];
    const auto playerIndex = static_cast<int>(player);
    const std::uint64_t identityGeneration =
        controllerMap_ == nullptr ? 0 : controllerMap_->generation(playerIndex);
    const auto stableId =
        identityGeneration == slot.identityGeneration
            ? slot.stableId
            : (controllerMap_ == nullptr
                   ? std::shared_ptr<const std::string>{}
                   : controllerMap_->stableId(playerIndex));
    XINPUT_STATE native{};
    const DWORD result = xinputGetState_ == nullptr
                             ? ERROR_DEVICE_NOT_CONNECTED
                             : xinputGetState_(player, &native);
    if (result != ERROR_SUCCESS || !stableId) {
      releaseController(slot, timestamp);
      slot.stableId = stableId;
      slot.connected = false;
      slot.hasObserved = false;
      slot.publishing = false;
      slot.identityGeneration = identityGeneration;
      return;
    }

    const WindowsXInputSample sample{
        .buttons = native.Gamepad.wButtons,
        .leftTrigger = native.Gamepad.bLeftTrigger,
        .rightTrigger = native.Gamepad.bRightTrigger,
        .leftX = native.Gamepad.sThumbLX,
        .leftY = native.Gamepad.sThumbLY,
        .rightX = native.Gamepad.sThumbRX,
        .rightY = native.Gamepad.sThumbRY};
    const auto current = windowsRealtimeControllerState(sample);
    const bool shouldPublish = controllerClaimActive_ && foregroundActive_;
    if (!slot.connected || slot.stableId != stableId || !slot.hasObserved) {
      releaseController(slot, timestamp);
      slot.stableId = stableId;
      slot.observed = current;
      slot.packetNumber = native.dwPacketNumber;
      slot.connected = true;
      slot.hasObserved = true;
      slot.publishing = shouldPublish;
      slot.identityGeneration = identityGeneration;
      return;
    }
    if (!shouldPublish) {
      releaseController(slot, timestamp);
      slot.observed = current;
      slot.packetNumber = native.dwPacketNumber;
      slot.publishing = false;
      return;
    }
    if (!slot.publishing) {
      slot.observed = current;
      slot.packetNumber = native.dwPacketNumber;
      slot.publishing = true;
      return;
    }
    if (slot.packetNumber == native.dwPacketNumber) {
      return;
    }
    publishControllerChanges(slot, current, timestamp);
    slot.observed = current;
    slot.packetNumber = native.dwPacketNumber;
  }

  void publishControllerChanges(ControllerSlot &slot,
                                const WindowsGameControllerState &current,
                                std::uint64_t timestamp) {
    for (std::size_t button = 0; button < current.buttons.size(); ++button) {
      if (slot.observed.buttons[button] == current.buttons[button]) {
        continue;
      }
      if (current.buttons[button]) {
        publishControllerButton(*slot.stableId, static_cast<int>(button), true,
                                timestamp);
        slot.published.buttons[button] = true;
      } else if (slot.published.buttons[button]) {
        publishControllerButton(*slot.stableId, static_cast<int>(button),
                                false, timestamp);
        slot.published.buttons[button] = false;
      }
    }
    for (std::size_t axis = 0; axis < current.axes.size(); ++axis) {
      if (slot.observed.axes[axis] == current.axes[axis]) {
        continue;
      }
      publishControllerAxis(*slot.stableId, static_cast<int>(axis),
                            current.axes[axis], timestamp);
      slot.published.axes[axis] = current.axes[axis];
    }
  }

  void publishControllerButton(const std::string &stableId, int button,
                               bool pressed, std::uint64_t timestamp) {
    publishInput({.control = {.deviceId = stableId,
                              .deviceClass =
                                  input::DeviceClass::GameController,
                              .kind = input::ControlKind::Button,
                              .index = button},
                  .rawValue = pressed ? 1.0 : 0.0,
                  .normalizedValue = pressed ? 1.0F : 0.0F,
                  .timestampMicros = timestamp});
  }

  void publishControllerAxis(const std::string &stableId, int axis,
                             std::int16_t value, std::uint64_t timestamp) {
    const float normalized = value < 0
                                 ? static_cast<float>(value) / 32768.0F
                                 : static_cast<float>(value) / 32767.0F;
    publishInput({.control = {.deviceId = stableId,
                              .deviceClass =
                                  input::DeviceClass::GameController,
                              .kind = input::ControlKind::Axis,
                              .index = axis},
                  .rawValue = static_cast<double>(value),
                  .normalizedValue = normalized,
                  .timestampMicros = timestamp});
  }

  void releaseController(ControllerSlot &slot, std::uint64_t timestamp) {
    if (!slot.stableId) {
      slot.published = {};
      return;
    }
    for (std::size_t button = 0; button < slot.published.buttons.size();
         ++button) {
      if (slot.published.buttons[button]) {
        publishControllerButton(*slot.stableId, static_cast<int>(button),
                                false, timestamp);
      }
    }
    for (std::size_t axis = 0; axis < slot.published.axes.size(); ++axis) {
      if (slot.published.axes[axis] != 0) {
        publishControllerAxis(*slot.stableId, static_cast<int>(axis), 0,
                              timestamp);
      }
    }
    slot.published = {};
  }

  void releaseControllers(std::uint64_t timestamp) {
    for (auto &slot : controllers_) {
      releaseController(slot, timestamp);
      slot.publishing = false;
    }
  }

  inline static std::atomic<WindowsRealtimeInputBackend *> hookOwner_ =
      nullptr;

  std::shared_ptr<RealtimeControllerDeviceMap> controllerMap_;
  QpcTimeline timeline_;
  std::array<bool, SDL_NUM_SCANCODES> physicalKeys_{};
  std::array<bool, SDL_NUM_SCANCODES> publishedKeys_{};
  std::array<ControllerSlot, RealtimeControllerDeviceMap::kMaxPlayers>
      controllers_{};
  std::atomic_bool keyboardClaimRequested_ = false;
  std::atomic_bool controllerClaimRequested_ = false;
  std::atomic<std::uint64_t> requestedClaimGeneration_ = 0;
  std::atomic<std::uint64_t> appliedClaimGeneration_ = 0;
  std::atomic<HRESULT> workerStartResult_ = E_PENDING;
  std::atomic<HRESULT> completionResult_ = E_PENDING;
  std::atomic<CompletionOperation> completionOperation_ =
      CompletionOperation::None;
  RtwqCallback *completionCallback_ = nullptr;
  RtwqCallback *workerCallback_ = nullptr;
  IRtwqAsyncResult *workerResult_ = nullptr;
  HMODULE xinputModule_ = nullptr;
  XInputGetStateFunction xinputGetState_ = nullptr;
  HHOOK keyboardHook_ = nullptr;
  HANDLE stopEvent_ = nullptr;
  HANDLE wakeEvent_ = nullptr;
  HANDLE claimAppliedEvent_ = nullptr;
  HANDLE completionEvent_ = nullptr;
  HANDLE workerReadyEvent_ = nullptr;
  HANDLE workerDoneEvent_ = nullptr;
  HANDLE timer_ = nullptr;
  DWORD workQueueId_ = 0;
  DWORD mmcssTaskId_ = 0;
  bool keyboardClaimActive_ = false;
  bool controllerClaimActive_ = false;
  bool foregroundActive_ = false;
  bool rtwqStarted_ = false;
  bool workQueueAllocated_ = false;
  bool mmcssRegistered_ = false;
  bool workerSubmitted_ = false;
  bool started_ = false;
};

} // namespace

std::unique_ptr<IInputBackend> makeWindowsRealtimeInputBackend(
    input::InputBackendSink sink,
    std::shared_ptr<RealtimeControllerDeviceMap> controllerMap) {
  return std::make_unique<WindowsRealtimeInputBackend>(
      std::move(sink), std::move(controllerMap));
}

#endif
