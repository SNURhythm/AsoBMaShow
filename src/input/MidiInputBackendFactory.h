#pragma once

#include "IInputBackend.h"

#include <memory>

std::unique_ptr<IInputBackend>
makeMidiInputBackend(input::InputBackendSink sink);
