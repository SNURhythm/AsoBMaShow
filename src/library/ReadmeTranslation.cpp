#include "ReadmeTranslation.h"
#include "DocumentLinks.h"
#include <mutex>
#include <unordered_map>

namespace readme_translation {
std::vector<Part> standardParts(std::string_view text) {
  const auto links = document_links::find(text);
  if (links.empty()) return {{std::string(text), true}};
  std::vector<Part> parts;
  const auto literal = [&](std::string_view value) {
    if (value.empty()) return;
    if (!parts.empty() && !parts.back().translate) parts.back().text += value;
    else parts.push_back({std::string(value), false});
  };
  const auto prose = [&](std::string_view value) {
    if (value.find_first_not_of(" \t\r\n.,;:!?()[]{}<>\"'") == std::string_view::npos) {
      literal(value); return;
    }
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n") + 1;
    literal(value.substr(0, first));
    parts.push_back({std::string(value.substr(first, last - first)), true});
    literal(value.substr(last));
  };
  std::size_t position = 0;
  for (const auto &link : links) {
    prose(text.substr(position, link.begin - position));
    literal(text.substr(link.begin, link.end - link.begin));
    position = link.end;
  }
  prose(text.substr(position));
  return parts;
}
struct Request::Mailbox { std::optional<Result> result; };
namespace {
std::mutex mutex;
std::uint64_t nextId = 0;
std::unordered_map<std::uint64_t, std::weak_ptr<Request::Mailbox>> requests;
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
