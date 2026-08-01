#pragma once

#include "IInputBackend.h"

#include <memory>

std::unique_ptr<IInputBackend>
makeIOSGyroscopeInputBackend(input::InputBackendSink sink);
std::unique_ptr<IInputBackend>
makeAndroidGyroscopeInputBackend(input::InputBackendSink sink);
std::unique_ptr<IInputBackend>
makeUnsupportedGyroscopeInputBackend(input::InputBackendSink sink);
