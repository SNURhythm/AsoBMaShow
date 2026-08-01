#pragma once

#include "IInputBackend.h"
#include "RealtimeControllerDeviceMap.h"

#include <memory>

std::unique_ptr<IInputBackend> makeWindowsRealtimeInputBackend(
    input::InputBackendSink sink,
    std::shared_ptr<RealtimeControllerDeviceMap> controllerMap);
