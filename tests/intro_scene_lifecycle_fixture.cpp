#include "input/InputDeviceRegistry.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

class Backend final : public IInputBackend {
public:
  explicit Backend(input::InputBackendSink sink) : IInputBackend(std::move(sink)) {}
  bool start(std::string &) override { return true; }
  void stop() override {}
  void pump() override {}
  void send() {
    publishInput({.control = {.deviceId = "keyboard", .deviceClass = input::DeviceClass::Keyboard,
                             .kind = input::ControlKind::Key}});
    publishDevice({.stableId = "keyboard", .deviceClass = input::DeviceClass::Keyboard,
                   .connected = false});
  }
};

// Forward delivery and cancellation to the real registry; observe only the
// scene's registrations and allow failure before the second registration.
struct ObservedRegistry {
  Backend *backend = nullptr;
  InputDeviceRegistry registry{{[this](input::InputBackendSink sink) {
    auto result = std::make_unique<Backend>(std::move(sink));
    backend = result.get();
    return result;
  }}};
  std::map<std::uint64_t, std::string> active;
  std::vector<std::string> events;
  int inputs = 0, disconnects = 0;
  bool failDevices = false;
  std::uint64_t subscribeInput(InputDeviceRegistry::InputListener listener) {
    const auto token = registry.subscribeInput(std::move(listener));
    active.emplace(token, "input");
    return token;
  }
  std::uint64_t subscribeDevices(InputDeviceRegistry::DeviceListener listener) {
    if (failDevices) throw std::runtime_error("device subscription failed");
    const auto token = registry.subscribeDevices(std::move(listener));
    active.emplace(token, "device");
    return token;
  }
  void unsubscribe(std::uint64_t token) {
    registry.unsubscribe(token);
    require(active.contains(token), "each owned subscription should be removed only once");
    events.push_back(active.at(token));
    active.erase(token);
  }
};
struct ApplicationContext {
  ObservedRegistry inputDeviceRegistry;
  ObservedRegistry &inputProfile = inputDeviceRegistry;
  struct { int skinMusicSelectInput = 0; } settings;
};
int musicSelectKeyLayoutForConfig(int configuration) { return configuration; }

class MusicSelectInputBindingAdapter {
public:
  MusicSelectInputBindingAdapter(ObservedRegistry &registry, int) : registry_(registry) {}
  ~MusicSelectInputBindingAdapter() {
    require(registry_.active.empty(), "Intro adapter destroyed with live input subscriptions");
    registry_.events.push_back("adapter");
  }
  void consume(const input::PhysicalInputEvent &) { ++registry_.inputs; }
  void disconnectDevice(const std::string &) { ++registry_.disconnects; }
  void reset() { registry_.events.push_back("reset"); }
private:
  ObservedRegistry &registry_;
};

struct View {
  explicit View(ObservedRegistry &registry) : registry(registry) {}
  virtual ~View() {
    require(registry.active.empty(), "Intro views must outlive input detachment");
    registry.events.push_back("view");
  }
  ObservedRegistry &registry;
};
class Scene {
public:
  explicit Scene(ApplicationContext &context) : context(context) {}
  BASE_METHODS
  std::vector<View *> views;
  std::map<Uint64, std::pair<Uint64, std::vector<std::function<bool()>>>> deferred;
protected:
  virtual void cleanupScene() = 0;
  ApplicationContext &context;
private:
  std::mutex postedDeferredMutex_;
  std::vector<std::function<bool()>> postedDeferred_;
  bool isDead = false, isCleaned = false;
};
class IntroScene final : public Scene {
public:
  explicit IntroScene(ApplicationContext &context) : Scene(context) {}
  ~IntroScene() override;
  void cleanupScene() override;
  void startInputListening();
  void stopInputListening();
  View *rootLayout_ = nullptr, *startButton_ = nullptr, *settingsButton_ = nullptr;
  std::unique_ptr<MusicSelectInputBindingAdapter> inputBindingAdapter_;
  std::uint64_t inputSubscription_ = 0, inputDeviceSubscription_ = 0;
  int layoutWidth_ = -1, layoutHeight_ = -1;
};
INTRO_METHODS

void checkLifetime(bool initialized, bool cleanupFirst, bool failDuringInit) {
  ApplicationContext context;
  auto &observed = context.inputDeviceRegistry;
  int unrelatedInputs = 0;
  const auto unrelated = observed.registry.subscribeInput([&](const auto &) { ++unrelatedInputs; });
  auto scene = std::make_unique<IntroScene>(context);
  scene->rootLayout_ = new View(observed);
  scene->views.push_back(scene->rootLayout_);
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> weakCapture = capture;
  scene->deferred[0].second.push_back([capture] { return false; });
  capture.reset();
  if (failDuringInit) {
    observed.failDevices = true;
    bool failed = false;
    try {
      auto pending = std::move(scene);
      pending->startInputListening();
    } catch (const std::runtime_error &) { failed = true; }
    require(failed, "second subscription should exercise initialization unwinding");
  } else if (initialized) {
    scene->startInputListening();
    scene->startInputListening();
    require(observed.active.size() == 2, "repeated start must not duplicate subscriptions");
    observed.backend->send();
    observed.registry.pump();
    require(observed.inputs == 1 && observed.disconnects == 1, "live scene receives input and disconnects");
    if (cleanupFirst) {
      scene->cleanup();
      const auto events = observed.events;
      scene->cleanup();
      require(observed.events == events, "guarded cleanup is idempotent");
    }
  }
  observed.backend->send(); // These queued events must not reach the destroyed scene.
  scene.reset();
  require(observed.active.empty() && weakCapture.expired(), "destruction releases registrations and captures");
  const auto inputs = observed.inputs, disconnects = observed.disconnects;
  const auto unrelatedBefore = unrelatedInputs;
  observed.registry.pump();
  require(observed.inputs == inputs && observed.disconnects == disconnects,
          "queued events cannot reach the released adapter");
  require(unrelatedInputs == unrelatedBefore + 1, "unrelated listener remains registered");
  observed.registry.unsubscribe(unrelated);
  const std::vector<std::string> expected = failDuringInit
      ? std::vector<std::string>{"input", "reset", "adapter", "view"}
      : initialized ? std::vector<std::string>{"input", "device", "reset", "adapter", "view"}
                    : std::vector<std::string>{"view"};
  require(observed.events == expected, "input, adapter, and view teardown order stays exact");
}
int main() {
  checkLifetime(true, false, false);
  checkLifetime(true, true, false);
  checkLifetime(false, false, false);
  checkLifetime(true, false, true);
}
