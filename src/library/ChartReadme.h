#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace chart_readme {
inline constexpr std::size_t maximumBytes = 1024U * 1024U;
inline constexpr std::size_t maximumDocuments = 1024;
enum class Encoding { Auto, Utf8, Japanese, Korean };
enum class Error { None, Cancelled, Unavailable, TooManyDocuments, TooLarge, Encoding };
struct Documents {
  std::vector<std::filesystem::path> paths;
  Error error = Error::None;
  std::string detail;
};
struct Content {
  std::string text;
  Error error = Error::None;
  std::string detail;
};
Documents list(const std::filesystem::path &chart, std::stop_token stop = {});
Content read(const std::filesystem::path &document, std::stop_token stop = {},
             Encoding encoding = Encoding::Auto);
Content decode(std::string_view bytes, Encoding encoding = Encoding::Auto);
std::vector<std::string> wrapLines(std::string_view text, int width,
    const std::function<int(const std::string &)> &measure);
// One worker per reader, with replacement/cancellation and nonblocking teardown.
// The worker owns only copied paths and state, never UI objects.
class Loader {
public:
  struct Result { Documents documents; Content content; bool discovered = false; };
  Loader();
  ~Loader();
  Loader(const Loader &) = delete;
  Loader &operator=(const Loader &) = delete;
  void open(std::filesystem::path chart, Encoding encoding = Encoding::Auto);
  void select(std::filesystem::path document, Encoding encoding = Encoding::Auto);
  void cancel();
  std::optional<Result> takeResult();
private:
  struct State;
  std::shared_ptr<State> state_;
  void request(std::filesystem::path path, bool discover, Encoding encoding);
};
} // namespace chart_readme
