#pragma once

#include "IInputBackend.h"

#include <memory>

std::unique_ptr<IInputBackend>
makeCoreMidiInputBackend(input::InputBackendSink sink);
std::unique_ptr<IInputBackend>
makeWinMidiInputBackend(input::InputBackendSink sink);
std::unique_ptr<IInputBackend>
makeAndroidMidiInputBackend(input::InputBackendSink sink);
std::unique_ptr<IInputBackend>
makeUnsupportedMidiInputBackend(input::InputBackendSink sink);
