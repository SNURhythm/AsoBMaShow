#include "CurlRAII.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

using WriteCallback = size_t (*)(char *, size_t, size_t, void *);
using ProgressCallback = int (*)(void *, curl_off_t, curl_off_t, curl_off_t,
                                curl_off_t);

struct Wire {
  WriteCallback write = nullptr;
  ProgressCallback progress = nullptr;
  void *writeContext = nullptr;
  void *progressContext = nullptr;
  std::vector<std::string> chunks;
  std::atomic_bool cancelled{false};
  size_t accepted = 0;
  size_t retained = 0;
  size_t cancelBefore = std::numeric_limits<size_t>::max();
  long status = 200;
  long post = 0;
  long noProgress = 1;
  bool overflow = false;
  bool rejected = false;
  bool idleCancel = false;
  std::string protocols;
  std::string redirectProtocols;
};

Wire wire;
size_t retainedBytes(void *context);

CURL *fixtureInit() { return reinterpret_cast<CURL *>(&wire); }
void fixtureCleanup(CURL *) {}
CURLcode fixtureGlobalInit(long) { return CURLE_OK; }
using FixtureHandle = std::unique_ptr<CURL, decltype(&fixtureCleanup)>;
struct FixtureEasyHandle : FixtureHandle {
  explicit FixtureEasyHandle(CURL *handle)
      : FixtureHandle(handle, fixtureCleanup) {}
};

template <typename Value>
CURLcode fixtureSetopt(CURL *, CURLoption option, Value value) {
  if constexpr (std::is_same_v<Value, WriteCallback>) {
    if (option == CURLOPT_WRITEFUNCTION) wire.write = value;
  } else if constexpr (std::is_same_v<Value, ProgressCallback>) {
    if (option == CURLOPT_XFERINFOFUNCTION) wire.progress = value;
  } else if constexpr (std::is_pointer_v<Value> &&
                       !std::is_function_v<std::remove_pointer_t<Value>>) {
    if (option == CURLOPT_WRITEDATA)
      wire.writeContext = const_cast<void *>(static_cast<const void *>(value));
    if (option == CURLOPT_XFERINFODATA)
      wire.progressContext = const_cast<void *>(static_cast<const void *>(value));
    if constexpr (std::is_convertible_v<Value, const char *>) {
      if (option == CURLOPT_PROTOCOLS_STR) wire.protocols = value;
      if (option == CURLOPT_REDIR_PROTOCOLS_STR) wire.redirectProtocols = value;
    }
  } else if constexpr (std::is_integral_v<Value>) {
    if (option == CURLOPT_POST) wire.post = value;
    if (option == CURLOPT_NOPROGRESS) wire.noProgress = value;
  }
  return CURLE_OK;
}

CURLcode fixtureGetinfo(CURL *, CURLINFO info, long *output) {
  if (info == CURLINFO_RESPONSE_CODE) *output = wire.status;
  return CURLE_OK;
}

CURLcode fixturePerform(CURL *) {
  if (wire.write == nullptr) return CURLE_FAILED_INIT;
  if (wire.idleCancel) {
    wire.cancelled.store(true);
    if (wire.progress != nullptr && wire.noProgress == 0 &&
        wire.progress(wire.progressContext, 0, 0, 0, 0) != 0)
      return CURLE_ABORTED_BY_CALLBACK;
    return CURLE_OK;
  }
  if (wire.overflow) {
    char tiny[] = "x";
    const auto returned = wire.write(tiny,
        std::numeric_limits<size_t>::max() / 2 + 1, 2, wire.writeContext);
    wire.retained = retainedBytes(wire.writeContext);
    wire.rejected = returned == 0;
    return CURLE_WRITE_ERROR;
  }
  for (size_t index = 0; index < wire.chunks.size(); ++index) {
    if (index == wire.cancelBefore) wire.cancelled.store(true);
    auto &chunk = wire.chunks[index];
    const auto returned = wire.write(chunk.data(), 1, chunk.size(),
                                     wire.writeContext);
    wire.retained = std::max(wire.retained, retainedBytes(wire.writeContext));
    wire.accepted += returned;
    if (returned != chunk.size()) {
      wire.rejected = true;
      return CURLE_WRITE_ERROR;
    }
  }
  return CURLE_OK;
}

#undef curl_easy_setopt
#undef curl_easy_getinfo
#define curl_easy_init fixtureInit
#define curl_easy_cleanup fixtureCleanup
#define curl_global_init fixtureGlobalInit
#define curl_easy_setopt fixtureSetopt
#define curl_easy_getinfo fixtureGetinfo
#define curl_easy_perform fixturePerform
#define CurlEasyHandle FixtureEasyHandle

namespace asobmshow::bms_search {
#include "transport_metadata_methods.inc"
}

#undef curl_easy_init
#undef curl_easy_cleanup
#undef curl_global_init
#undef curl_easy_setopt
#undef curl_easy_getinfo
#undef curl_easy_perform
#undef CurlEasyHandle

namespace actual_curl {
#include "transport_metadata_methods.inc"
}

size_t retainedBytes(void *context) {
#if TRANSPORT_HAS_RECEIVE_CONTEXT
  return static_cast<asobmshow::bms_search::CurlTextResponseContext *>(context)
      ->body.size();
#else
  return static_cast<std::string *>(context)->size();
#endif
}

template <typename Fetch>
std::optional<std::string> invoke(Fetch fetch, std::string &error) {
  if constexpr (std::is_invocable_v<Fetch, const std::string &, std::string &,
                                    const std::atomic_bool *, size_t>) {
    return fetch("https://fixture.invalid/metadata", error, &wire.cancelled, 16);
  } else {
    return fetch("https://fixture.invalid/metadata", error);
  }
}

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void reset() {
  wire.write = nullptr;
  wire.progress = nullptr;
  wire.writeContext = nullptr;
  wire.progressContext = nullptr;
  wire.chunks.clear();
  wire.cancelled.store(false);
  wire.accepted = wire.retained = 0;
  wire.cancelBefore = std::numeric_limits<size_t>::max();
  wire.status = 200;
  wire.post = 0;
  wire.noProgress = 1;
  wire.overflow = wire.rejected = false;
  wire.idleCancel = false;
  wire.protocols.clear();
  wire.redirectProtocols.clear();
}

template <typename Fetch>
void exercise(Fetch fetch, const std::string &method) {
  reset();
  wire.chunks = {"{\"ok\":", "true}"};
  std::string error;
  const auto normal = invoke(fetch, error);
  expect(normal && *normal == "{\"ok\":true}", method + " normal complete body");
  expect(wire.post == (method == "POST" ? 1 : 0), method + " registration");
  expect(wire.protocols == "http,https" && wire.redirectProtocols == "https",
         method + " preserves initial HTTPS redirect restriction");
  reset();
  wire.chunks = {"123456789abcdefgh"};
  error.clear();
  expect(!invoke(fetch, error) && wire.retained == 0 && wire.rejected,
         method + " oversized first chunk rejected without retaining prefix");
  for (const long status : {200L, 500L}) {
    reset();
    wire.status = status;
    wire.chunks = {"12345678", "abcdefgh", "!"};
    error.clear();
    const auto oversized = invoke(fetch, error);
    std::cout << method << " status=" << status
              << " success=" << oversized.has_value()
              << " accepted=" << wire.accepted << " retained=" << wire.retained
              << " error=" << error << '\n';
    expect(!oversized, method + " oversized body never returns partial success");
    expect(wire.rejected && wire.accepted == 16 && wire.retained == 16,
           method + " no Content-Length: reject crossing chunk before append");
    expect(error.find("limit") != std::string::npos,
           method + " size failure distinct from HTTP/transport error");
  }
  reset();
  wire.chunks = {"prefix", "suffix"};
  wire.cancelBefore = 1;
  error.clear();
  expect(!invoke(fetch, error), method + " cancelled response has no value");
  expect(wire.accepted == 6 && wire.retained == 6 && wire.rejected,
         method + " cancelled callback stops before append");
  expect(error.find("cancel") != std::string::npos,
         method + " cancellation diagnostic");
  reset();
  wire.cancelled.store(true);
  wire.chunks = {"body"};
  error.clear();
  expect(!invoke(fetch, error) && wire.retained == 0,
         method + " pre-cancelled response retains nothing");
  reset();
  wire.idleCancel = true;
  error.clear();
  expect(!invoke(fetch, error) && wire.retained == 0 &&
             error.find("cancel") != std::string::npos,
         method + " registered progress callback interrupts idle receive");
  reset();
  wire.overflow = true;
  error.clear();
  expect(!invoke(fetch, error) && wire.retained == 0,
         method + " size multiplication overflow retains nothing");
  expect(error.find("limit") != std::string::npos,
         method + " wrapped multiplication cannot masquerade as empty chunk");
  reset();
  wire.status = 503;
  wire.chunks = {"unavailable"};
  error.clear();
  expect(!invoke(fetch, error) && error.find("503") != std::string::npos,
         method + " normal HTTP error preserved");
  reset();
  wire.chunks = {"12345678", "abcdefgh"};
  error.clear();
  const auto exact = invoke(fetch, error);
  expect(exact && *exact == "12345678abcdefgh", method + " exact limit admitted");
  reset();
  wire.chunks = {"", "ok", ""};
  error = "previous request failed";
  const auto emptyChunks = invoke(fetch, error);
  expect(emptyChunks && *emptyChunks == "ok" && error.empty(),
         method + " zero-length chunks preserve successful body and clear error");
}

template <typename Fetch>
void exerciseLoopback(Fetch fetch, const std::string &method,
                       const std::string &origin) {
  std::atomic_bool cancelled{false};
  for (const std::string route : {"normal", "exact", "large", "error",
                                  "no-length", "chunked"}) {
    const std::string url = origin + "/metadata/" + route;
    std::string error;
    const auto body = [&] {
      if constexpr (std::is_invocable_v<Fetch, const std::string &, std::string &,
                                        const std::atomic_bool *, size_t>)
        return fetch(url, error, &cancelled, 16);
      else
        return fetch(url, error);
    }();
    const bool normal = route == "normal" || route == "exact";
    expect(body.has_value() == normal, method + " real curl " + route);
    if (normal)
      expect(body && *body == (route == "normal" ? method : "12345678abcdefgh"),
             method + " real curl complete method/body " + route);
    else
      expect(error.find("limit") != std::string::npos,
             method + " real curl bounded response error " + route);
  }
}

int main(int argc, char **argv) {
  exercise(asobmshow::bms_search::fetchUrlText, "GET");
  exercise(asobmshow::bms_search::postUrlText, "POST");
  if (argc == 2) {
    const std::string origin = argv[1];
    if (!origin.starts_with("http://127.0.0.1:")) return 2;
    exerciseLoopback(actual_curl::fetchUrlText, "GET", origin);
    exerciseLoopback(actual_curl::postUrlText, "POST", origin);
  }
  std::cout << "metadata production callback failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
