#include "ChartReadme.h"
#include "../ArchiveFile.h"
#include "../Utils.h"
#include "../input/Utf16ToUtf8.h"
#include "../text/Utf8.h"
#include "../targets.h"
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif
#if !TARGET_OS_ANDROID && !defined(_WIN32) && __has_include(<iconv.h>)
#include <iconv.h>
#define CHART_README_HAS_ICONV 1
#endif
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace chart_readme {
namespace {
std::optional<std::string> decodeLegacy(std::string_view bytes, Encoding encoding) {
  const bool korean = encoding == Encoding::Korean;
#if TARGET_OS_ANDROID
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  if (!env) return std::nullopt;
  if (env->PushLocalFrame(8) < 0) { env->ExceptionClear(); return std::nullopt; }
  const auto converted = [&]() -> std::optional<std::string> {
    const auto input = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (!input) return std::nullopt;
    env->SetByteArrayRegion(input, 0, static_cast<jsize>(bytes.size()),
                           reinterpret_cast<const jbyte *>(bytes.data()));
    if (env->ExceptionCheck()) return std::nullopt;
    const auto type = env->FindClass("java/lang/String");
    if (!type) return std::nullopt;
    const auto constructor = env->GetMethodID(type, "<init>", "([BLjava/lang/String;)V");
    if (!constructor) return std::nullopt;
    const auto charset = env->NewStringUTF(korean ? "MS949" : "MS932");
    if (!charset) return std::nullopt;
    const auto decoded = static_cast<jstring>(env->NewObject(type, constructor, input, charset));
    if (!decoded || env->ExceptionCheck()) return std::nullopt;
    const auto length = env->GetStringLength(decoded);
    const auto *units = env->GetStringChars(decoded, nullptr);
    if (!units) return std::nullopt;
    const auto result = utf16ToUtf8(std::u16string_view(
        reinterpret_cast<const char16_t *>(units), static_cast<std::size_t>(length)));
    env->ReleaseStringChars(decoded, units);
    // Neither legacy charset encodes U+FFFD. Java inserts it for invalid bytes;
    // reject that substitution so Auto can try the other legacy encoding.
    if (result.find("\xef\xbf\xbd") != std::string::npos) return std::nullopt;
    return result;
  }();
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->PopLocalFrame(nullptr);
  return converted;
#elif defined(_WIN32)
  const auto codePage = korean ? 949U : 932U;
  const int length = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
      bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
  if (length <= 0) return bytes.empty() ? std::optional<std::string>("") : std::nullopt;
  std::wstring units(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(),
      static_cast<int>(bytes.size()), units.data(), length) != length) return std::nullopt;
  return utf16ToUtf8(std::u16string(units.begin(), units.end()));
#elif CHART_README_HAS_ICONV
  const auto converter = iconv_open("UTF-8", korean ? "CP949" : "CP932");
  if (converter == reinterpret_cast<iconv_t>(-1)) return std::nullopt;
  std::string output(std::max<std::size_t>(32, bytes.size() * 4), '\0');
  auto *input = const_cast<char *>(bytes.data());
  auto remaining = bytes.size();
  auto *next = output.data();
  auto available = output.size();
  const auto status = iconv(converter, &input, &remaining, &next, &available);
  iconv_close(converter);
  if (status == static_cast<std::size_t>(-1) || remaining != 0) return std::nullopt;
  output.resize(output.size() - available);
  return output;
#else
  return std::nullopt;
#endif
}
bool textFile(const std::filesystem::path &path) {
  auto name = fspath_to_utf8(path.filename());
  for (auto &c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return name.ends_with(".txt");
}
}
Documents list(const std::filesystem::path &chart, std::stop_token stop) {
  Documents result;
  if (stop.stop_requested()) return {.error = Error::Cancelled};
  if (chart.empty()) return {.error = Error::Unavailable};
  const auto append = [&](const std::filesystem::path &path) {
    if (!textFile(path)) return true;
    if (result.paths.size() == maximumDocuments) {
      result.error = Error::TooManyDocuments;
      return false;
    }
    result.paths.push_back(path);
    return true;
  };
  std::filesystem::path archive, inner;
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(chart)) {
    if (!ListAndroidTreeTextFiles(chart.parent_path(), result.paths, result.detail))
      result.error = Error::Unavailable;
    else if (result.paths.size() > maximumDocuments) result.error = Error::TooManyDocuments;
  } else
#endif
  if (archive_file::splitVirtualPath(chart, archive, inner)) {
    const bool listed = archive_file::visitIndexEntries(archive, [&](const auto &entry) {
      if (stop.stop_requested()) return false;
      return entry.directory || entry.path.parent_path() != inner.parent_path() ||
             append(archive_file::makeVirtualPath(archive, entry.path));
    }, &result.detail, [&] { return !stop.stop_requested(); });
    if (!listed && result.error == Error::None) result.error = Error::Unavailable;
  } else {
    std::error_code error;
    auto folder = chart.parent_path();
    if (folder.empty()) folder = ".";
    std::filesystem::directory_iterator it(folder, error), end;
    while (!error && it != end && !stop.stop_requested()) {
      if (it->is_regular_file(error) && !error && !append(it->path())) break;
      if (!error) it.increment(error);
    }
    if (error) { result.error = Error::Unavailable; result.detail = error.message(); }
  }
  if (stop.stop_requested()) result.error = Error::Cancelled;
  if (result.error != Error::None) result.paths.clear();
  std::ranges::sort(result.paths, [](const auto &left, const auto &right) {
    return fspath_to_utf8(left.filename()) < fspath_to_utf8(right.filename());
  });
  return result;
}

Content decode(std::string_view bytes, Encoding encoding) {
  if (bytes.size() > maximumBytes) return {.error = Error::TooLarge};
  std::string text;
  const bool littleEndian = bytes.starts_with("\xff\xfe");
  if (littleEndian || bytes.starts_with("\xfe\xff")) {
    bytes.remove_prefix(2);
    if (bytes.size() % 2 != 0) return {.error = Error::Encoding};
    std::u16string units;
    units.reserve(bytes.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
      const auto a = static_cast<unsigned char>(bytes[i]);
      const auto b = static_cast<unsigned char>(bytes[i + 1]);
      units.push_back(static_cast<char16_t>(littleEndian ? a | (b << 8) : (a << 8) | b));
    }
    for (std::size_t i = 0; i < units.size(); ++i) {
      if (units[i] >= 0xd800 && units[i] <= 0xdbff) {
        if (++i == units.size() || units[i] < 0xdc00 || units[i] > 0xdfff)
          return {.error = Error::Encoding};
      } else if (units[i] >= 0xdc00 && units[i] <= 0xdfff) return {.error = Error::Encoding};
    }
    text = utf16ToUtf8(units);
  } else {
    if (bytes.find('\0') != std::string_view::npos) return {.error = Error::Encoding};
    const bool utf8Bom = bytes.starts_with("\xef\xbb\xbf");
    if (utf8Bom) bytes.remove_prefix(3);
    const bool forceLegacy = !utf8Bom &&
        (encoding == Encoding::Japanese || encoding == Encoding::Korean);
    if (!forceLegacy && asobmashow::text::validUtf8(bytes)) text.assign(bytes);
    else if (!utf8Bom && encoding != Encoding::Utf8) {
      auto converted = decodeLegacy(bytes, encoding == Encoding::Korean ? Encoding::Korean : Encoding::Japanese);
      if (!converted && encoding == Encoding::Auto) converted = decodeLegacy(bytes, Encoding::Korean);
      if (!converted) return {.error = Error::Encoding};
      text = *converted;
    } else return {.error = Error::Encoding};
  }
  if (text.find('\0') != std::string::npos) return {.error = Error::Encoding};
  Content result;
  result.text.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\r') {
      result.text += '\n';
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
    } else if (c == '\t') result.text += "    ";
    else if (static_cast<unsigned char>(c) >= 0x20 || c == '\n') result.text += c;
  }
  return result;
}

Content read(const std::filesystem::path &document, std::stop_token stop, Encoding encoding) {
  if (stop.stop_requested()) return {.error = Error::Cancelled};
  // A regular-file check also prevents a .txt pipe from blocking the reader.
  if (!archive_file::isVirtualPath(document)) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(document, error)) return {.error = Error::Unavailable};
    const auto size = std::filesystem::file_size(document, error);
    if (!error && size > maximumBytes) return {.error = Error::TooLarge};
  }
  std::vector<unsigned char> bytes;
  std::string error;
  if (!archive_file::readFileBounded(document, bytes, maximumBytes, &error, stop)) {
    // ArchiveFile reports the same limit rejection for indexed archive sizes
    // and streaming platform reads. Preserve it instead of hiding the limit
    // behind a generic I/O error (these diagnostics are not localized).
    const bool tooLarge = error.starts_with("File exceeds bounded read limit") ||
                          error.starts_with("Archive entry exceeds bounded read limit");
    return {.error = stop.stop_requested() ? Error::Cancelled :
                     tooLarge ? Error::TooLarge : Error::Unavailable, .detail = std::move(error)};
  }
  if (stop.stop_requested()) return {.error = Error::Cancelled};
  return decode(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), encoding);
}

std::vector<std::string> wrapLines(std::string_view text, int width,
    const std::function<int(const std::string &)> &measure) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  do {
    const auto newline = text.find('\n', start);
    const auto end = newline == std::string_view::npos ? text.size() : newline;
    const auto line = text.substr(start, end - start);
    if (line.empty()) lines.emplace_back();
    else {
      std::vector<std::size_t> boundaries{0};
      for (std::size_t i = 1; i < line.size(); ++i)
        if ((static_cast<unsigned char>(line[i]) & 0xc0) != 0x80) boundaries.push_back(i);
      boundaries.push_back(line.size());
      for (std::size_t first = 0; first + 1 < boundaries.size();) {
        const auto last = std::min(boundaries.size() - 1, first + 4096);
        std::size_t low = first + 1, high = low, fit = low;
        // Find a nearby upper bound; searching half of the entire remaining
        // document for every row makes a long unbroken line quadratic.
        while (high < last) {
          if (measure(std::string(line.substr(boundaries[first], boundaries[high] - boundaries[first]))) > std::max(1, width)) break;
          fit = high;
          low = high + 1;
          high = std::min(last, first + (high - first) * 2);
        }
        while (low <= high) {
          const auto middle = low + (high - low) / 2;
          if (measure(std::string(line.substr(boundaries[first], boundaries[middle] - boundaries[first]))) <= std::max(1, width)) {
            fit = middle; low = middle + 1;
          } else high = middle - 1;
        }
        if (fit + 1 < boundaries.size()) {
          for (auto split = fit; split > first + 1; --split) {
            if (line[boundaries[split] - 1] == ' ') { fit = split; break; }
          }
        }
        lines.emplace_back(line.substr(boundaries[first], boundaries[fit] - boundaries[first]));
        first = fit;
      }
    }
    if (newline == std::string_view::npos) break;
    start = newline + 1;
  } while (start <= text.size());
  return lines;
}
struct Loader::State {
  struct Request { std::filesystem::path path; bool discover; std::uint64_t generation; Encoding encoding; };
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<Request> pending;
  std::optional<Result> result;
  std::stop_source active;
  std::uint64_t generation = 0;
  bool quitting = false;
};
Loader::Loader() : state_(std::make_shared<State>()) {
  std::thread([state = state_] {
    for (;;) {
      State::Request request;
      std::stop_token stop;
      {
        std::unique_lock lock(state->mutex);
        state->ready.wait(lock, [&] { return state->quitting || state->pending.has_value(); });
        if (state->quitting) return;
        request = std::move(*state->pending);
        state->pending.reset();
        state->active = std::stop_source{};
        stop = state->active.get_token();
      }
      Result result;
      result.discovered = request.discover;
      try {
        if (request.discover) {
          result.documents = list(request.path, stop);
          if (result.documents.error == Error::None && !result.documents.paths.empty())
            result.content = read(result.documents.paths.front(), stop, request.encoding);
        } else result.content = read(request.path, stop, request.encoding);
      } catch (const std::exception &error) {
        result.content = {.error = Error::Unavailable, .detail = error.what()};
      } catch (...) { result.content.error = Error::Unavailable; }
      std::lock_guard lock(state->mutex);
      if (!state->quitting && request.generation == state->generation)
        state->result = std::move(result);
    }
  }).detach();
}
Loader::~Loader() {
  { std::lock_guard lock(state_->mutex);
    state_->quitting = true;
    state_->active.request_stop();
    state_->pending.reset();
  }
  state_->ready.notify_one();
}
void Loader::request(std::filesystem::path path, bool discover, Encoding encoding) {
  { std::lock_guard lock(state_->mutex);
    state_->active.request_stop();
    state_->result.reset();
    state_->pending = State::Request{std::move(path), discover, ++state_->generation, encoding};
  }
  state_->ready.notify_one();
}
void Loader::open(std::filesystem::path chart, Encoding encoding) { request(std::move(chart), true, encoding); }
void Loader::select(std::filesystem::path document, Encoding encoding) { request(std::move(document), false, encoding); }
void Loader::cancel() {
  std::lock_guard lock(state_->mutex);
  ++state_->generation;
  state_->active.request_stop();
  state_->pending.reset();
  state_->result.reset();
}
std::optional<Loader::Result> Loader::takeResult() {
  std::lock_guard lock(state_->mutex);
  auto result = std::move(state_->result);
  state_->result.reset();
  return result;
}
} // namespace chart_readme
