#include "GyroscopePlatformBackends.h"

#include <memory>
#include <utility>

namespace {

class UnsupportedGyroscopeInputBackend final : public IInputBackend {
public:
  explicit UnsupportedGyroscopeInputBackend(input::InputBackendSink sink)
      : IInputBackend(std::move(sink)) {}

  bool start(std::string &errorMessage) override {
    errorMessage.clear();
    return true;
  }
  void stop() override {}
  void pump() override {}
};

} // namespace

std::unique_ptr<IInputBackend>
makeUnsupportedGyroscopeInputBackend(input::InputBackendSink sink) {
  return std::make_unique<UnsupportedGyroscopeInputBackend>(std::move(sink));
}
