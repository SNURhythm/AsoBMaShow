#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace readme_translation {
inline constexpr std::size_t maximumOutputBytes = 4 * 1024 * 1024;
enum class State { Preparing, Working, Complete, Failed, Cancelled };
struct Result { State state; std::string text; };
class Request {
public:
  struct Mailbox;
  Request();
  ~Request();
  Request(const Request &) = delete;
  Request &operator=(const Request &) = delete;
  std::uint64_t begin();
  void cancel();
  std::optional<Result> take();
  std::uint64_t id() const { return id_; }
private:
  std::shared_ptr<Mailbox> mailbox_;
  std::uint64_t id_ = 0;
};
// Thread-safe delivery. Unknown, cancelled and completed IDs are ignored.
void deliver(std::uint64_t id, State state, std::string text);
bool supported();
bool qualitySelectionSupported();
void start(std::uint64_t id, const std::string &text, const std::string &target,
           const std::string &source = "auto", bool highFidelity = false);
void stop(std::uint64_t id);
}
