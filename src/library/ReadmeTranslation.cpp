#include "ReadmeTranslation.h"
#include <algorithm>
#include <mutex>
#include <numeric>
#include <unordered_map>

namespace readme_translation {
struct Request::Mailbox { std::optional<Result> result; };
namespace {
std::mutex mutex;
std::uint64_t nextId = 0;
std::unordered_map<std::uint64_t, std::weak_ptr<Request::Mailbox>> requests;
}
std::vector<std::string> chunks(std::string_view text) {
  std::vector<std::string> result;
  while (!text.empty()) {
    if (text.front() == '\n' || text.front() == ' ' || text.front() == '\t') {
      const auto end = std::min({text.size(), text.find_first_not_of(" \t\n"), std::size_t(2000)});
      result.emplace_back(text.substr(0, end)); text.remove_prefix(end); continue;
    }
    auto end = std::min({text.size(), text.find('\n'), std::size_t(2000)});
    while (end < text.size() && end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    // Prefer a word boundary for long lines, without dropping whitespace.
    if (end < text.size() && text[end] != '\n') {
      const auto space = text.substr(0, end).find_last_of(" \t");
      if (space != std::string_view::npos && space > end / 2) end = space + 1;
    }
    const auto contentEnd = text.substr(0, end).find_last_not_of(" \t") + 1;
    result.emplace_back(text.substr(0, contentEnd));
    if (contentEnd < end) result.emplace_back(text.substr(contentEnd, end - contentEnd));
    text.remove_prefix(end);
  }
  return result;
}
std::vector<std::size_t> languageSamples(const std::vector<std::string> &parts) {
  std::vector<std::size_t> samples(parts.size());
  std::iota(samples.begin(), samples.end(), 0);
  const auto nonAscii = [](const std::string &part) {
    return std::ranges::any_of(part, [](unsigned char c) { return c >= 0x80; });
  };
  // Short mixed-script credits (for example Artist님) are unreliable language
  // samples. Borrow longer non-ASCII prose from the same paragraph. Keep each
  // actual inference chunk and all paragraph/line boundaries unchanged.
  std::size_t begin = 0;
  while (begin < parts.size()) {
    std::size_t end = begin, representative = begin;
    for (; end < parts.size(); ++end) {
      if (std::count(parts[end].begin(), parts[end].end(), '\n') >= 2) break;
      if (nonAscii(parts[end]) &&
          (!nonAscii(parts[representative]) || parts[end].size() > parts[representative].size()))
        representative = end;
    }
    if (nonAscii(parts[representative]) && parts[representative].size() >= 64) {
      for (auto i = begin; i < end; ++i) {
        const bool latin = std::ranges::any_of(parts[i], [](unsigned char c) {
          return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        });
        if (parts[i].size() < 64 && latin && nonAscii(parts[i])) samples[i] = representative;
      }
    }
    begin = end + 1;
  }
  return samples;
}
Request::Request() : mailbox_(std::make_shared<Mailbox>()) {}
Request::~Request() { cancel(); }
std::uint64_t Request::begin() {
  std::lock_guard lock(mutex);
  requests.erase(id_);
  mailbox_->result.reset();
  id_ = ++nextId;
  requests[id_] = mailbox_;
  return id_;
}
void Request::cancel() {
  std::lock_guard lock(mutex);
  requests.erase(id_); id_ = 0; mailbox_->result.reset();
}
std::optional<Result> Request::take() {
  std::lock_guard lock(mutex);
  auto result = std::move(mailbox_->result); mailbox_->result.reset(); return result;
}
void deliver(std::uint64_t id, State state, std::string text) {
  std::lock_guard lock(mutex);
  const auto found = requests.find(id);
  if (found == requests.end()) return;
  if (state == State::Complete && text.size() > maximumOutputBytes) {
    state = State::Failed;
    text.clear();
  }
  if (auto box = found->second.lock()) box->result = Result{state, std::move(text)};
  if (state == State::Complete || state == State::Failed || state == State::Cancelled)
    requests.erase(found);
}
}
