#include "GyroscopeInputBackendFactory.h"

#include "GyroscopePlatformBackends.h"

#include <utility>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

std::unique_ptr<IInputBackend>
makeGyroscopeInputBackend(input::InputBackendSink sink) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  return makeIOSGyroscopeInputBackend(std::move(sink));
#elif defined(__ANDROID__)
  return makeAndroidGyroscopeInputBackend(std::move(sink));
#else
  return makeUnsupportedGyroscopeInputBackend(std::move(sink));
#endif
}
