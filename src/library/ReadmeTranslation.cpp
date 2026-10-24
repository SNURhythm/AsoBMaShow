#include "ReadmeTranslation.h"
#include <mutex>
#include <unordered_map>

namespace readme_translation {
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
