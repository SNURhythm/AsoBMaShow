#pragma once

#include "IInputBackend.h"

#include <memory>

std::unique_ptr<IInputBackend>
makeGyroscopeInputBackend(input::InputBackendSink sink);
