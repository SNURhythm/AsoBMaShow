#include "DesktopAudioRoute.h"
#include "../targets.h"

#if TARGET_OS_OSX
#include <CoreAudio/CoreAudio.h>
#include <pa_mac_core.h>
#elif TARGET_OS_WINDOWS
#include <initguid.h>
#include <audiopolicy.h>
#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <cfgmgr32.h>
#include <wrl/client.h>
#include <string_view>
#elif TARGET_OS_LINUX
#include "LinuxAudioRoute.h"
#endif
#if TARGET_OS_WINDOWS || TARGET_OS_LINUX
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace audio {
#if TARGET_OS_WINDOWS
namespace {
using Microsoft::WRL::ComPtr;
std::optional<bool> endpointBluetooth(IMMDevice *endpoint) {
  // Follow the endpoint's topology to its hardware adapter before walking PnP
  // ancestry. Endpoint instance IDs themselves are SWD\\MMDEVAPI, even for BT.
  ComPtr<IDeviceTopology> topology;
  ComPtr<IConnector> connector;
  if (FAILED(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void **>(topology.GetAddressOf()))) ||
      FAILED(topology->GetConnector(0, &connector))) return std::nullopt;
  LPWSTR adapterId = nullptr;
  if (FAILED(connector->GetDeviceIdConnectedTo(&adapterId))) return std::nullopt;
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IMMDevice> adapter;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
  if (SUCCEEDED(result)) result = enumerator->GetDevice(adapterId, &adapter);
  CoTaskMemFree(adapterId);
  if (FAILED(result)) return std::nullopt;
  ComPtr<IPropertyStore> properties;
  if (FAILED(adapter->OpenPropertyStore(STGM_READ, &properties))) return std::nullopt;
  PROPVARIANT value{};
  result = properties->GetValue(PKEY_Device_InstanceId, &value);
  DEVINST node = 0;
  const bool located = SUCCEEDED(result) && value.vt == VT_LPWSTR &&
      CM_Locate_DevNodeW(&node, value.pwszVal, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
  PropVariantClear(&value);
  if (!located) return std::nullopt;
  for (int depth = 0; depth < 32; ++depth) {
    WCHAR id[MAX_DEVICE_ID_LEN]{};
    if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return std::nullopt;
    const std::wstring_view instance(id);
    if (instance.starts_with(L"BTHENUM\\") || instance.starts_with(L"BTHLEDEVICE\\") ||
        instance.starts_with(L"BTHHFENUM\\") || instance.starts_with(L"BTH\\")) return true;
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) return false;
    node = parent;
  }
  return std::nullopt;
}
std::optional<bool> processOutputBluetooth() {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized)) return std::nullopt;
  struct Cleanup { ~Cleanup() { CoUninitialize(); } } cleanup;
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IMMDeviceCollection> endpoints;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator))) ||
      FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints)))
    return std::nullopt;
  UINT count = 0;
  if (FAILED(endpoints->GetCount(&count))) return std::nullopt;
  struct Routes {
    bool seen = false, ambiguous = false;
    std::optional<bool> type;
    void add(std::optional<bool> next) {
      seen = true;
      if (!next || (type && type != next)) ambiguous = true;
      if (next) type = next;
    }
    std::optional<bool> result() const { return ambiguous ? std::nullopt : type; }
  } active, inactive;
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> endpoint;
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(endpoints->Item(i, &endpoint)) ||
        FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void **>(manager.GetAddressOf()))) ||
        FAILED(manager->GetSessionEnumerator(&sessions))) continue;
    int sessionCount = 0;
    if (FAILED(sessions->GetCount(&sessionCount))) continue;
    for (int j = 0; j < sessionCount; ++j) {
      ComPtr<IAudioSessionControl> control;
      ComPtr<IAudioSessionControl2> session;
      DWORD pid = 0;
      AudioSessionState state{};
      if (FAILED(sessions->GetSession(j, &control)) || FAILED(control.As(&session)) ||
          FAILED(session->GetProcessId(&pid)) || pid != GetCurrentProcessId() ||
          FAILED(session->GetState(&state)) || state == AudioSessionStateExpired) continue;
      (state == AudioSessionStateActive ? active : inactive).add(endpointBluetooth(endpoint.Get()));
    }
  }
  return active.seen ? active.result() : inactive.result();
}
}
#elif TARGET_OS_LINUX
namespace {
std::optional<bool> processOutputBluetooth() { return linuxProcessOutputIsBluetooth(); }
}
#endif

#if TARGET_OS_WINDOWS || TARGET_OS_LINUX
namespace {
class ProcessRouteMonitor {
public:
  ProcessRouteMonitor() : worker_([this] {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
      const auto request = requested_;
      lock.unlock();
      try {
        const auto type = processOutputBluetooth();
        value_.store(type ? (*type ? 1 : 0) : -1);
      } catch (...) {
        value_.store(-1);
      }
      lock.lock();
      completed_ = request;
      wake_.notify_all();
      wake_.wait_for(lock, std::chrono::milliseconds(500),
                     [this] { return stopping_ || requested_ != completed_; });
    }
  }) {}
  ~ProcessRouteMonitor() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    wake_.notify_all();
    worker_.join();
  }
  std::optional<bool> value(bool refresh) {
    if (refresh) {
      std::unique_lock lock(mutex_);
      const auto request = ++requested_;
      wake_.notify_all();
      // Only launch/attempt boundaries request a fresh sample. Never use a
      // pre-request result after a route changed during the previous song.
      if (!wake_.wait_for(lock, std::chrono::milliseconds(300),
                          [this, request] { return completed_ >= request; })) return std::nullopt;
    }
    const int value = value_.load();
    return value < 0 ? std::nullopt : std::optional<bool>(value != 0);
  }
private:
  std::atomic<int> value_{-1};
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  unsigned long long requested_ = 0, completed_ = 0;
  std::thread worker_;
};
}
#endif

std::optional<bool> desktopOutputIsBluetooth(void *stream, bool refresh) {
  if (!stream) return std::nullopt;
#if TARGET_OS_OSX
  (void)refresh;
  const AudioDeviceID device = PaMacCore_GetStreamOutputDevice(stream);
  if (device == kAudioObjectUnknown) return std::nullopt;
  AudioObjectPropertyAddress property{kAudioDevicePropertyTransportType,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  UInt32 transport = 0;
  UInt32 size = sizeof(transport);
  if (AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, &transport) != noErr)
    return std::nullopt;
  if (transport == kAudioDeviceTransportTypeAggregate ||
      transport == kAudioDeviceTransportTypeVirtual) return std::nullopt;
  return transport == kAudioDeviceTransportTypeBluetooth ||
         transport == kAudioDeviceTransportTypeBluetoothLE;
#elif TARGET_OS_WINDOWS || TARGET_OS_LINUX
  // Keep observing while gameplay defers selection, so the next song does
  // not consume a stale pre-song query. Routine polling never blocks the UI.
  static ProcessRouteMonitor monitor;
  return monitor.value(refresh);
#else
  return std::nullopt;
#endif
}
}
