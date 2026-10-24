#include "skin/beatoraja/LuaSkinAsyncHttp.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
using namespace skin;
using namespace std::chrono_literals;
namespace {
int failures = 0;
void expect(bool value, const char *message) { if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  int entered = 0;
  int destroyed = 0;
  bool released = false;
  void release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
  bool waitEntered(int count) { std::unique_lock lock(mutex); return changed.wait_for(lock, 2s, [&] { return entered >= count; }); }
  bool waitDestroyed(int count) { std::unique_lock lock(mutex); return changed.wait_for(lock, 2s, [&] { return destroyed >= count; }); }
};
struct Connection : LuaSkinHttpConnection {
  std::shared_ptr<Gate> gate;
  int code;
  std::string body;
  Connection(std::shared_ptr<Gate> g, int c, std::string b) : gate(std::move(g)), code(c), body(std::move(b)) {}
  ~Connection() override { std::lock_guard lock(gate->mutex); ++gate->destroyed; gate->changed.notify_all(); }
  std::optional<std::string> connect() noexcept override {
    std::unique_lock lock(gate->mutex); ++gate->entered; gate->changed.notify_all();
    gate->changed.wait(lock, [&] { return gate->released; }); return {};
  }
  LuaSkinHttpCodeResult responseCode() noexcept override { return {.code = code}; }
  LuaSkinHttpBodyResult readBody() noexcept override { return {.body = body}; }
  void disconnect() noexcept override {}
};
struct Transport : LuaSkinHttpTransport {
  std::shared_ptr<Gate> gate = std::make_shared<Gate>();
  int code = 200;
  std::string body = "a\r\nb\n";
  LuaSkinHttpOpenResult open(std::string_view, int, LuaSkinHttpLimits) override {
    return {.connection = std::make_unique<Connection>(gate, code, body)};
  }
};
LuaSkinAsyncHttpResult terminal(LuaSkinAsyncHttp &http, int id) {
  const auto end = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < end) {
    auto result = http.poll(id);
    if (result.status != "pending") return result;
    std::this_thread::yield();
  }
  return {.status = "pending"};
}
void lifecycle() {
  Transport transport;
  LuaSkinAsyncHttp http(&transport);
  expect(!http.submit("http://user:pass@example.com/").id, "credentials rejected on submission");
  expect(!http.submit("https://example.com", 1000, 0).id, "invalid raw limit rejected");
  auto request = http.submit("https://example.com", 1000, 8);
  expect(request.id.has_value() && http.reservedBytes() == 8, "submission reserves raw capacity");
  if (!request.id) return;
  expect(transport.gate->waitEntered(1), "worker enters connection");
  expect(http.poll(*request.id).status == "pending", "pending poll does not consume");
  transport.gate->release();
  const auto end = std::chrono::steady_clock::now() + 2s;
  while (http.reservedBytes() != 5 && std::chrono::steady_clock::now() < end) std::this_thread::yield();
  expect(http.reservedBytes() == 5, "completion shrinks reservation to actual raw body before polling");
  auto result = terminal(http, *request.id);
  expect(result.status == "completed" && result.statusCode == 200 && result.body == "a\r\nb\n", "completed result retains status and raw body");
  expect(http.reservedBytes() == 0 && http.poll(*request.id).status == "failed" && !http.cancel(*request.id), "terminal result consumes exactly once");
  transport.code = 404;
  auto missing = http.submit("https://example.com");
  auto failed = terminal(http, *missing.id);
  expect(failed.status == "failed" && failed.statusCode == 404 && failed.body.empty(), "non-2xx preserves HTTP status without body");
  transport.code = 200;
  auto oversized = http.submit("https://example.com", 1000, 3);
  expect(terminal(http, *oversized.id).status == "failed", "owner enforces byte limits even for injected transport");
}
void capacityAndDisposal() {
  Transport transport;
  auto http = std::make_unique<LuaSkinAsyncHttp>(&transport);
  auto first = http->submit("https://example.com", 5000, 8388608);
  auto second = http->submit("https://example.com", 5000, 8388608);
  expect(first.id && second.id && transport.gate->waitEntered(2), "both shared workers run");
  expect(!http->submit("https://example.com", 5000, 1).id, "16 MiB reservation budget rejects excess");
  expect(http->cancel(*first.id) && http->cancel(*second.id), "running requests cancel independently");
  expect(http->reservedBytes() == 16777216 && !http->submit("https://example.com", 5000, 1).id, "cancelled running workers keep reservations until exit");
  auto start = std::chrono::steady_clock::now();
  http.reset();
  expect(std::chrono::steady_clock::now() - start < 100ms, "owner destruction never joins workers");
  transport.gate->release();
  expect(transport.gate->waitDestroyed(2), "connections survive owner then are released by workers");
}
void boundedQueueAndDeadlines() {
  Transport transport;
  LuaSkinAsyncHttp running(&transport), first(&transport), second(&transport), extra(&transport);
  auto a = running.submit("https://example.com", 5000, 1);
  auto b = running.submit("https://example.com", 5000, 1);
  expect(a.id && b.id && transport.gate->waitEntered(2), "queue fixture occupies two workers");
  auto deadline = extra.submit("https://example.com", 20, 1);
  expect(deadline.id.has_value(), "queued deadline request accepted");
  auto result = terminal(extra, *deadline.id);
  expect(result.status == "failed" && result.error.find("timed out") != std::string::npos && extra.reservedBytes() == 0, "deadline includes queue and releases queued reservation");
  std::vector<int> ids;
  for (int i = 0; i < 16; ++i) {
    auto one = first.submit("https://example.com", 5000, 1);
    auto two = second.submit("https://example.com", 5000, 1);
    expect(one.id && two.id, "32 shared queue slots accepted");
    if (one.id) ids.push_back(*one.id);
  }
  expect(!first.submit("https://example.com", 5000, 1).id, "per-skin 16 retained limit enforced");
  expect(!extra.submit("https://example.com", 5000, 1).id, "global queue rejects 33rd waiting job");
  expect(first.cancel(ids.back()), "queued request can be removed");
  expect(extra.submit("https://example.com", 5000, 1).id.has_value(), "queued cancellation immediately frees queue slot");
  first.dispose(); second.dispose(); extra.dispose();
  expect(first.reservedBytes() == 0 && !first.submit("https://example.com").id, "disposed owner releases pending capacity and rejects submissions");
  transport.gate->release();
}
void runningDeadline() {
  Transport transport;
  LuaSkinAsyncHttp http(&transport);
  auto request = http.submit("https://example.com", 30, 8);
  expect(request.id && transport.gate->waitEntered(1), "deadline fixture starts a running request");
  auto result = terminal(http, *request.id);
  expect(result.status == "failed" && result.error.find("timed out") != std::string::npos,
         "running deadline publishes terminal failure before blocked worker exits");
  expect(http.reservedBytes() == 8, "consumed timeout still reserves running capacity");
  transport.gate->release();
  const auto end = std::chrono::steady_clock::now() + 2s;
  while (http.reservedBytes() && std::chrono::steady_clock::now() < end) std::this_thread::yield();
  expect(http.reservedBytes() == 0, "worker completion releases consumed timeout reservation");
}
void transportLifetime() {
  auto transport = std::make_unique<Transport>();
  auto gate = transport->gate;
  LuaSkinAsyncHttp http(transport.get());
  auto id = http.submit("https://example.com");
  expect(id.id && gate->waitEntered(1), "owned connection starts");
  transport.reset(); gate->release();
  expect(terminal(http, *id.id).status == "completed", "worker owns connection without retaining raw transport");
}
}
int main() {
  lifecycle(); capacityAndDisposal(); boundedQueueAndDeadlines(); runningDeadline(); transportLifetime();
  if (failures) return 1;
  std::cout << "Lua skin async HTTP tests passed\n";
}
