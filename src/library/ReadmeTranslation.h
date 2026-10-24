#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace readme_translation {
inline constexpr std::size_t maximumOutputBytes = 4 * 1024 * 1024;
enum class State { Preparing, Working, Complete, Failed, Cancelled };
struct Result { State state; std::string text; };
struct Part { std::string text; bool translate; };
// Standard models receive prose separately; URLs and boundary whitespace stay verbatim.
std::vector<Part> standardParts(std::string_view text);
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
