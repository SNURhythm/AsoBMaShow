#pragma once

#include "IInputBackend.h"
#include "RealtimeControllerDeviceMap.h"
#include <memory>

std::unique_ptr<IInputBackend> makeDesktopRealtimeKeyboardBackend(
    input::InputBackendSink sink,
    std::shared_ptr<RealtimeControllerDeviceMap> deviceMap);
