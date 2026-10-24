#include "skin/beatoraja/LuaSkinCurlHttpTransport.h"
#include "skin/beatoraja/LuaSkinHttpClient.h"
#if defined(ASOBMASHOW_TEST_CURL_HTTP_TRANSPORT)
#include "skin/beatoraja/LuaSkinAsyncHttp.h"
#include "skin/beatoraja/LuaSkinCurlHttpTransportTest.h"
#endif
#if defined(ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT)
#include "skin/beatoraja/LuaSkinFoundationHttpTransportTest.h"
#endif

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <iostream>
#include <limits>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket invalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
using Socket = int;
constexpr Socket invalidSocket = -1;
#endif

namespace {

using namespace skin;
using namespace std::chrono_literals;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void closeSocket(Socket socket) noexcept {
  if (socket == invalidSocket) {
    return;
  }
#if defined(_WIN32)
  closesocket(socket);
#else
  ::close(socket);
#endif
}

#if defined(ASOBMASHOW_TEST_CURL_HTTP_TRANSPORT)
// Receive real DNS queries without ever replying. No public resolver, HTTP
// server, sleeping resolver thread, or destination-policy bypass is involved.
class BlackholeDnsServer {
public:
  BlackholeDnsServer() {
#if defined(_WIN32)
    WSADATA data{};
    startedSockets_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    if (!startedSockets_) return;
#endif
    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == invalidSocket) return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (::bind(socket_, reinterpret_cast<sockaddr *>(&address), size) != 0 ||
        ::getsockname(socket_, reinterpret_cast<sockaddr *>(&address), &size) != 0) return;
    port_ = ntohs(address.sin_port);
    setLuaSkinCurlDnsServersForTesting("127.0.0.1:" + std::to_string(port_));
  }

  ~BlackholeDnsServer() {
    setLuaSkinCurlDnsServersForTesting({});
    closeSocket(socket_);
#if defined(_WIN32)
    if (startedSockets_) WSACleanup();
#endif
  }

  bool ready() const noexcept { return port_ != 0; }

  bool waitForQueries(std::string_view first, std::string_view second) {
    bool firstSeen = false, secondSeen = false;
    const auto end = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < end) {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(socket_, &readable);
      timeval timeout{0, 10000};
      if (::select(static_cast<int>(socket_ + 1), &readable, nullptr, nullptr, &timeout) <= 0) continue;
      std::array<unsigned char, 512> packet{};
      const auto size = ::recv(socket_, reinterpret_cast<char *>(packet.data()),
                               static_cast<int>(packet.size()), 0);
      if (size < 17 || (packet[2] & 0x80) != 0) continue;
      std::string host;
      std::size_t offset = 12;
      while (offset < static_cast<std::size_t>(size) && packet[offset] != 0) {
        const auto length = packet[offset++];
        if (length > 63 || offset + length >= static_cast<std::size_t>(size)) break;
        if (!host.empty()) host += '.';
        host.append(reinterpret_cast<const char *>(packet.data() + offset), length);
        offset += length;
      }
      firstSeen = firstSeen || host == first;
      secondSeen = secondSeen || host == second;
      if (firstSeen && secondSeen) return true;
    }
    return false;
  }

private:
  Socket socket_ = invalidSocket;
  std::uint16_t port_ = 0;
#if defined(_WIN32)
  bool startedSockets_ = false;
#endif
};

template <typename Predicate>
bool waitForHttpCleanup(Predicate ready) {
  const auto end = std::chrono::steady_clock::now() + 750ms;
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= end) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

void testStalledDnsReleasesAsyncWorkers() {
  BlackholeDnsServer dns;
  expect(dns.ready(), "local UDP DNS blackhole starts");
  if (!dns.ready()) return;
  auto transport = createLuaSkinProductionHttpTransport();
  for (bool cancel : {true, false}) {
    LuaSkinAsyncHttp owner(transport.get());
    const std::string firstHost = cancel ? "cancel-first.test" : "deadline-first.test";
    const std::string secondHost = cancel ? "cancel-second.test" : "deadline-second.test";
    const int timeout = cancel ? 5000 : 300;
    const auto first = owner.submit("http://" + firstHost + "/", timeout, 64);
    const auto second = owner.submit("http://" + secondHost + "/", timeout, 64);
    expect(first.id && second.id, "both native DNS requests accepted");
    if (!first.id || !second.id) return;
    // Distinct QNAMEs prove both shared workers entered DNS. Merely observing
    // two packets could mistake one request's A/AAAA pair for two requests.
    const bool resolving = dns.waitForQueries(firstHost, secondHost);
    expect(resolving, "both native workers send queries to the DNS blackhole");
    if (!resolving) return;
    expect(owner.poll(*first.id).status == "pending" &&
           owner.poll(*second.id).status == "pending" && owner.reservedBytes() == 128,
           "unanswered DNS requests occupy both workers and retain capacity");

    // Queue behind both stalled requests on the same owner/native transport.
    // Reaching socket-policy rejection proves execution, unlike a timer result.
    const auto probe = owner.submit("http://127.0.0.1:9/", 2000, 1);
    expect(probe.id.has_value(), "subsequent native request queues behind DNS");
    if (!probe.id) return;
    if (cancel) {
      expect(owner.cancel(*first.id) && owner.cancel(*second.id),
             "both in-flight DNS requests cancel");
    } else {
      for (const int id : {*first.id, *second.id}) {
        LuaSkinAsyncHttpResult result;
        expect(waitForHttpCleanup([&] {
          result = owner.poll(id);
          return result.status != "pending";
        }) && result.status == "failed" && result.error.find("timed out") != std::string::npos,
               "total deadline reports timeout during real stalled DNS");
      }
    }
    LuaSkinAsyncHttpResult result;
    expect(waitForHttpCleanup([&] {
      result = owner.poll(*probe.id);
      return result.status != "pending";
    }) && result.status == "failed" && result.error.find("Non-public") != std::string::npos,
           "released DNS worker executes the queued request with destination policy intact");
    expect(waitForHttpCleanup([&] { return owner.reservedBytes() == 0; }),
           "both DNS connections finish cleanup and release reserved capacity");
  }
}
#endif

class LoopbackHttpServer {
public:
  LoopbackHttpServer() {
#if defined(_WIN32)
    WSADATA data{};
    startedSockets_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    if (!startedSockets_) {
      return;
    }
#endif
    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == invalidSocket) {
      return;
    }
    int enabled = 1;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&enabled), sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 8) != 0) {
      closeSocket(listener_);
      listener_ = invalidSocket;
      return;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                      &size) != 0) {
      closeSocket(listener_);
      listener_ = invalidSocket;
      return;
    }
    port_ = ntohs(address.sin_port);
    thread_ = std::jthread([this](std::stop_token stop) { serve(stop); });
  }

  ~LoopbackHttpServer() {
    thread_.request_stop();
    closeSocket(listener_);
    listener_ = invalidSocket;
    if (thread_.joinable()) {
      thread_.join();
    }
    workers_.clear();
#if defined(_WIN32)
    if (startedSockets_) {
      WSACleanup();
    }
#endif
  }

  [[nodiscard]] bool ready() const noexcept {
    return listener_ != invalidSocket && port_ != 0;
  }

  [[nodiscard]] std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
  }

  [[nodiscard]] std::string virtualHostUrl(std::string_view path) const {
    return "http://example.test:" + std::to_string(port_) + std::string(path);
  }
  std::size_t requests() const noexcept { return requests_.load(); }
  bool originalHostSeen() const noexcept { return originalHostSeen_.load(); }

  [[nodiscard]] std::size_t tailBytesSent() const noexcept {
    return tailBytesSent_.load();
  }

  [[nodiscard]] std::size_t stalledRequests() const noexcept {
    return stalledRequests_.load();
  }

private:
  static bool sendAll(Socket client, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#if defined(_WIN32)
      const int count = ::send(client, bytes.data() + sent,
                               static_cast<int>(bytes.size() - sent), 0);
#else
#if defined(MSG_NOSIGNAL)
      constexpr int sendFlags = MSG_NOSIGNAL;
#else
      constexpr int sendFlags = 0;
#endif
      const ssize_t count =
          ::send(client, bytes.data() + sent, bytes.size() - sent, sendFlags);
#endif
      if (count <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(count);
    }
    return true;
  }

  void respond(Socket client, std::string_view path) {
    if (path == "/first-stalls" && requests_.load() == 1) {
      std::unique_lock lock(requestMutex_);
      requestChanged_.wait_for(lock, 2s, [&] { return requests_.load() > 1; });
      return;
    }
    if (path == "/error") {
      (void)sendAll(client, "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
      return;
    }
    if (path == "/redirect") {
      (void)sendAll(client,
                    "HTTP/1.1 302 Found\r\nLocation: /ok\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n");
      return;
    }
    if (path == "/overflow" || path == "/large") {
      const std::string body(path == "/large" ? 1572864 : 1024 * 1024 + 1, 'a');
      const std::string header =
          "HTTP/1.1 200 OK\r\nContent-Length: " +
          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
      (void)sendAll(client, header);
      (void)sendAll(client, body);
      return;
    }
    if (path == "/unframed") {
      (void)sendAll(client, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
      (void)sendAll(client, std::string(4096, '\n'));
      return;
    }
    if (path == "/tail") {
      constexpr std::size_t tailSize = 1024 * 1024;
      const std::string prefix(1024 * 2, 'x');
      std::string lines;
      lines.reserve(prefix.size());
      for (int index = 0; index < 1024; ++index) {
        lines += "x\n";
      }
      const std::string header =
          "HTTP/1.1 200 OK\r\nContent-Length: " +
          std::to_string(lines.size() + tailSize) +
          "\r\nConnection: close\r\n\r\n";
      if (!sendAll(client, header) || !sendAll(client, lines)) {
        return;
      }
      const std::string chunk(4096, 'z');
      for (std::size_t sent = 0; sent < tailSize; sent += chunk.size()) {
        if (!sendAll(client, chunk)) {
          return;
        }
        tailBytesSent_.fetch_add(chunk.size());
        std::this_thread::sleep_for(1ms);
      }
      return;
    }
    if (path == "/stall") {
      // NSURLSession may defer publishing a response until an initial body chunk.
      // Leave the rest stalled so both adapters exercise a separate read phase.
      stalledRequests_.fetch_add(1);
      if (!sendAll(client,
                   "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 4100\r\n"
                   "Connection: close\r\n\r\n") || !sendAll(client, std::string(4096, 'x'))) {
        return;
      }
      std::this_thread::sleep_for(750ms);
      (void)sendAll(client, "late");
      return;
    }
    const std::string body = "alpha\r\nbeta\n";
    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    (void)sendAll(client, response);
  }

  void serve(std::stop_token stop) {
    while (!stop.stop_requested()) {
      sockaddr_in peer{};
      socklen_t size = sizeof(peer);
      const Socket client =
          ::accept(listener_, reinterpret_cast<sockaddr *>(&peer), &size);
      if (client == invalidSocket) {
        return;
      }
      workers_.emplace_back([this, client] { handle(client); });
    }
  }

  void handle(Socket client) {
#if defined(__APPLE__)
      int noSigPipe = 1;
      setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe,
                 sizeof(noSigPipe));
#endif
      std::string request;
      std::array<char, 4096> buffer{};
      while (request.find("\r\n\r\n") == std::string::npos) {
#if defined(_WIN32)
        const int count = ::recv(client, buffer.data(),
                                 static_cast<int>(buffer.size()), 0);
#else
        const ssize_t count = ::recv(client, buffer.data(), buffer.size(), 0);
#endif
        if (count <= 0) {
          break;
        }
        request.append(buffer.data(), static_cast<std::size_t>(count));
      }
      std::string_view path = "/";
      const std::size_t firstSpace = request.find(' ');
      if (firstSpace != std::string::npos) {
        const std::size_t secondSpace = request.find(' ', firstSpace + 1);
        if (secondSpace != std::string::npos) {
          path = std::string_view(request).substr(
              firstSpace + 1, secondSpace - firstSpace - 1);
        }
      }
      { std::lock_guard lock(requestMutex_); requests_.fetch_add(1); }
      requestChanged_.notify_all();
      if (request.find("Host: example.test:" + std::to_string(port_)) != std::string::npos)
        originalHostSeen_.store(true);
      respond(client, path);
      closeSocket(client);
  }

  Socket listener_ = invalidSocket;
  std::uint16_t port_ = 0;
  std::jthread thread_;
  std::vector<std::jthread> workers_;
  std::mutex requestMutex_;
  std::condition_variable requestChanged_;
  std::atomic_size_t requests_{0};
  std::atomic_bool originalHostSeen_{false};
  std::atomic_size_t tailBytesSent_{0};
  std::atomic_size_t stalledRequests_{0};
#if defined(_WIN32)
  bool startedSockets_ = false;
#endif
};

void testResponseCodeReturnsBeforeAStalledBody() {
  LoopbackHttpServer server;
  expect(server.ready(), "staged loopback HTTP server starts");
  if (!server.ready()) {
    return;
  }
  auto transport = createLuaSkinProductionHttpTransport();
  LuaSkinHttpClient client(transport.get());
  auto opened = client.open(server.url("/stall"), 200);
  expect(opened.connection != nullptr && !opened.failure,
         "stalled response opens a production connection");
  if (!opened.connection) {
    return;
  }
  expect(!opened.connection->connect(), "stalled response connects");
  const auto responseStarted = std::chrono::steady_clock::now();
  const auto response = opened.connection->responseCode();
  const auto responseElapsed = std::chrono::steady_clock::now() - responseStarted;
  expect(response.code && *response.code == 200 && !response.failure,
         "responseCode observes headers before the stalled body");
  expect(responseElapsed < 150ms,
         "responseCode does not spend the body read timeout");

  const auto readStarted = std::chrono::steady_clock::now();
  const auto body = opened.connection->readBody();
  const auto readElapsed = std::chrono::steady_clock::now() - readStarted;
  expect(body.failure && body.failure->find("timed out") != std::string::npos,
         "the stalled body timeout belongs to the read phase");
  expect(readElapsed >= 150ms,
         "the read phase independently waits for its bounded timeout");
  opened.connection->disconnect();
  opened.connection->disconnect();
  expect(server.stalledRequests() == 1,
         "staged response and body operations issue exactly one GET");
}

void testProductionTransportStagesRedirectsAndBoundsTail() {
  LoopbackHttpServer server;
  expect(server.ready(), "loopback HTTP server starts");
  if (!server.ready()) {
    return;
  }
  auto transport = createLuaSkinProductionHttpTransport();
  expect(transport != nullptr, "the production HTTP transport is available");
  if (!transport) {
    return;
  }
  LuaSkinHttpClient client(transport.get());
  const auto ok = client.get(server.url("/ok"), 1000);
  const auto okLines = ok.response
                           ? LuaSkinHttpClient::readLines(ok.response->body)
                           : LuaSkinHttpLinesResult{};
  expect(ok.response && ok.response->responseCode == 200 && !okLines.failure &&
             okLines.lines == std::vector<std::string>({"alpha", "beta"}),
         "production GET preserves the staged response and UTF-8 line body");

  const auto redirected = client.get(server.url("/redirect"), 1000);
  expect(redirected.response && redirected.response->responseCode == 200,
         "production GET follows only the configured HTTP redirect");

  const auto overflow = client.get(server.url("/overflow"), 1000);
  expect(overflow.failure && *overflow.failure == "response is too large",
         "production GET rejects the first raw byte over the default bound");

  const auto tail = client.get(server.url("/tail"), 1000);
  const auto tailLines = tail.response
                             ? LuaSkinHttpClient::readLines(tail.response->body)
                             : LuaSkinHttpLinesResult{};
  expect(tail.failure && *tail.failure == "response is too large",
         "production GET rejects oversized bodies even after many lines");
  std::this_thread::sleep_for(20ms);
  expect(server.tailBytesSent() <= 1024 * 1024,
         "the adapter closes before downloading an unbounded response tail");
}

void testProductionTransportHonorsCancellation() {
  std::stop_source stop;
  stop.request_stop();
  auto transport = createLuaSkinProductionHttpTransport(stop.get_token());
  LuaSkinHttpClient client(transport.get());
  const auto cancelled = client.get("http://127.0.0.1:9/cancelled", 5000);
  expect(cancelled.failure && *cancelled.failure == "HTTP request was cancelled",
         "production HTTP observes the owning session stop token");
}

void testStalledBodyCanBeCancelledAfterHeaders() {
  LoopbackHttpServer server;
  expect(server.ready(), "cancellation loopback HTTP server starts");
  if (!server.ready()) {
    return;
  }
  std::stop_source stop;
  auto transport = createLuaSkinProductionHttpTransport(stop.get_token());
  LuaSkinHttpClient client(transport.get());
  auto opened = client.open(server.url("/stall"), 5000);
  expect(opened.connection != nullptr && !opened.failure,
         "cancellable stalled response opens");
  if (!opened.connection) {
    return;
  }
  expect(!opened.connection->connect(), "cancellable stalled response connects");
  const auto response = opened.connection->responseCode();
  expect(response.code && *response.code == 200,
         "cancellable stalled response publishes headers");
  std::jthread cancel([&stop] {
    std::this_thread::sleep_for(30ms);
    stop.request_stop();
  });
  const auto body = opened.connection->readBody();
  expect(body.failure && *body.failure == "HTTP request was cancelled",
         "read-phase cancellation stops a stalled production body");
  opened.connection->disconnect();
  expect(server.stalledRequests() == 1,
         "cancelled staged request still sends only one GET");
}

void testRequestOptions() {
  LoopbackHttpServer server;
  auto transport = createLuaSkinProductionHttpTransport();
  std::stop_source one;
  LuaSkinHttpLimits limits;
  limits.requestStop = one.get_token();
  auto cancelled = transport->open(server.url("/ok"), 1000, limits);
  one.request_stop();
  expect(cancelled.connection->connect().has_value(), "per-request cancellation stops just its connection");
  LuaSkinHttpClient client(transport.get());
  expect(client.get(server.url("/ok"), 1000).response.has_value(), "one cancelled request leaves sibling requests usable");
  limits.requestStop = {};
  limits.deadline = std::chrono::steady_clock::now() - 1ms;
  auto expired = transport->open(server.url("/ok"), 1000, limits);
  expect(expired.connection->connect().has_value(), "expired total deadline prevents connection");
  limits.deadline = std::chrono::steady_clock::time_point::max();
  limits.restrictedDestinations = true;
  auto restricted = transport->open(server.url("/ok"), 1000, limits);
  auto failure = restricted.connection->connect();
  if (!failure) failure = restricted.connection->responseCode().failure;
  expect(failure && failure->find("Non-public") != std::string::npos, "async production transport rejects loopback destinations");
  expect(!client.get(server.url("/ok"), 1000, 12).failure, "body exactly at capacity accepted");
  for (bool afterHeaders : {false, true}) {
    auto larger = client.open(server.url("/large"), 1000);
    if (afterHeaders) expect(larger.connection->responseCode().code == 200, "large response publishes headers before applying body limit");
    larger.connection->setResponseLimitBytes(2097152);
    const auto body = larger.connection->readBody();
    expect(body.body && body.body->size() == 1572864 && !body.failure,
           afterHeaders ? "larger body limit applies after responseCode" : "larger body limit applies before connect");
  }
  auto tiny = client.open(server.url("/ok"), 1000, 1);
  expect(tiny.connection->responseCode().code == 200, "one-byte initial limit still permits headers");
  tiny.connection->setResponseLimitBytes(12);
  const auto expanded = tiny.connection->readBody();
  expect(expanded.body && expanded.body->size() == 12,
         "body delivery is deferred until the final response limit is selected");
  auto smaller = client.open(server.url("/ok"), 1000);
  expect(smaller.connection->responseCode().code == 200, "small response publishes headers");
  smaller.connection->setResponseLimitBytes(11);
  expect(smaller.connection->readBody().failure.has_value(), "smaller body limit applies after headers");

  expect(client.get(server.url("/ok"), 1000, 11).failure.has_value(), "body one byte over capacity rejected");
  const auto unframed = client.get(server.url("/unframed"), 1000, 4096);
  expect(unframed.response && unframed.response->body.size() == 4096 &&
         LuaSkinHttpClient::readLines(unframed.response->body).lines.size() == 4096,
         "unframed bodies retain all newline bytes without line truncation");
  expect(client.get(server.url("/unframed"), 1000, 4095).failure.has_value(),
         "streamed byte cap applies without Content-Length");
  limits.restrictedDestinations = false;
  limits.deadline = std::chrono::steady_clock::now() + 100ms;
  auto timed = transport->open(server.url("/stall"), 5000, limits);
  auto timedStart = std::chrono::steady_clock::now();
  auto timedBody = timed.connection->readBody();
  expect(timedBody.failure && std::chrono::steady_clock::now() - timedStart < 400ms,
         "total deadline bounds a stalled body independently of phase timeout");
  limits.deadline = std::chrono::steady_clock::time_point::max();
  std::stop_source requestStop;
  limits.requestStop = requestStop.get_token();
  auto stopped = transport->open(server.url("/stall"), 5000, limits);
  expect(stopped.connection->responseCode().code == 200, "per-request cancellation obtains response headers");
  requestStop.request_stop();
  expect(stopped.connection->readBody().failure.has_value(), "per-request cancellation interrupts a staged body");

  expect(!LuaSkinHttpClient::readLines("é\n", 3).failure && LuaSkinHttpClient::readLines("é\n", 2).failure,
         "UTF-8 and newline bytes count before decoding");
  expect(!LuaSkinHttpClient::checkedLimit(1.5) && !LuaSkinHttpClient::checkedLimit(0) &&
         !LuaSkinHttpClient::checkedLimit(8388609) && LuaSkinHttpClient::checkedLimit(8388608), "capacity validates integral hard boundary");
  expect(!LuaSkinHttpClient::checkedLimit(std::numeric_limits<double>::infinity()) &&
         !LuaSkinHttpClient::checkedLimit(std::numeric_limits<double>::quiet_NaN()), "nonfinite capacity rejected");
  expect(!LuaSkinHttpClient::validateAsyncRedirect("http://example.com", "https://example.com/path", 3),
         "third redirect and HTTP-to-HTTPS upgrade allowed");
  expect(LuaSkinHttpClient::validateAsyncRedirect("https://example.com", "http://example.com/path", 1).has_value(),
         "HTTPS downgrade rejected");
  expect(LuaSkinHttpClient::validateAsyncRedirect("https://example.com", "https://example.com/path", 4).has_value(),
         "fourth redirect rejected");
  expect(LuaSkinHttpClient::validateAsyncRedirect("https://example.com", "https://user:pass@example.com", 1).has_value(),
         "redirect destination credentials revalidated");
  for (const std::array<unsigned char, 4> address : {
      std::array<unsigned char, 4>{0, 1, 2, 3}, {100, 64, 1, 1}, {172, 16, 1, 1},
      {192, 0, 2, 1}, {198, 18, 0, 1}, {198, 51, 100, 1}, {203, 0, 113, 1}, {224, 0, 0, 1}})
    expect(!LuaSkinHttpClient::isPublicAddress(address), "non-public IPv4 range denied");
  for (const std::array<unsigned char, 4> address : {
      std::array<unsigned char, 4>{192, 0, 0, 0}, {192, 0, 0, 255},
      {192, 0, 2, 0}, {192, 0, 2, 255}, {198, 51, 100, 0}, {198, 51, 100, 255},
      {192, 168, 0, 0}, {192, 168, 255, 255}, {198, 18, 0, 0}, {198, 19, 255, 255}})
    expect(!LuaSkinHttpClient::isPublicAddress(address), "reserved IPv4 prefix endpoints denied");
  for (const std::array<unsigned char, 4> address : {
      std::array<unsigned char, 4>{8, 8, 8, 8}, {192, 0, 1, 0}, {192, 0, 1, 255},
      {192, 0, 3, 0}, {192, 0, 78, 17}, {198, 51, 99, 1}, {198, 51, 99, 255},
      {198, 51, 101, 0}})
    expect(LuaSkinHttpClient::isPublicAddress(address), "public IPv4 outside reserved prefixes accepted");
  std::array<unsigned char, 16> ipv6{0x20, 0x01, 0x48, 0x60};
  expect(LuaSkinHttpClient::isPublicAddress(ipv6), "global IPv6 accepted");
  ipv6[2] = 0x0d; ipv6[3] = 0xb8;
  expect(!LuaSkinHttpClient::isPublicAddress(ipv6), "documentation IPv6 denied");
  ipv6 = {}; ipv6[0] = 0x20; ipv6[1] = 0x02;
  expect(!LuaSkinHttpClient::isPublicAddress(ipv6), "transition IPv6 denied");
  for (auto url : {"ftp://example.com", "http://x:0", "http://x:65536", "http://user@x", "http://x/%zz", "http://[not-ipv6]"})
    expect(LuaSkinHttpClient::validateAsyncUrl(url).has_value(), "malformed async URLs rejected");
}

#if defined(ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT)
void testFoundationCandidateFallback() {
  LoopbackHttpServer server;
  expect(server.ready(), "candidate fallback loopback server starts");
  if (!server.ready()) return;
  LuaSkinHttpLimits limits;
  limits.restrictedDestinations = true;
  limits.deadline = std::chrono::steady_clock::now() + 2s;
  std::vector<std::size_t> attempts;
  auto connection = createLuaSkinFoundationCandidateTestConnection(server.virtualHostUrl("/ok"), limits,
      {.candidates = {"::1", "127.0.0.1"}, .beforeAttempt = [&](auto index) { attempts.push_back(index); }});
  const auto body = connection->readBody();
  expect(body.body && *body.body == "alpha\r\nbeta\n" && attempts == std::vector<std::size_t>({0, 1}),
         "refused first address falls back to second validated candidate");
  expect(server.requests() == 1 && server.originalHostSeen(), "candidate fallback retains original Host header");

  const auto base = std::chrono::steady_clock::now();
  auto simulatedNow = base;
  limits.deadline = base + 1s;
  const auto before = server.requests();
  attempts.clear();
  auto expired = createLuaSkinFoundationCandidateTestConnection(server.virtualHostUrl("/ok"), limits,
      {.candidates = {"::1", "127.0.0.1"},
       .now = [&] { return std::max(simulatedNow, std::chrono::steady_clock::now()); },
       .beforeAttempt = [&](auto index) {
         attempts.push_back(index);
         simulatedNow = index == 0 ? base + 900ms : base + 1001ms;
       }});
  const auto failure = expired->responseCode().failure;
  expect(failure && failure->find("timed out") != std::string::npos && server.requests() == before &&
         attempts == std::vector<std::size_t>({0, 1}),
         "fallback cannot reset original total deadline before opening second candidate");

  {
    LoopbackHttpServer stalled;
    attempts.clear();
    limits.deadline = std::chrono::steady_clock::now() + 500ms;
    auto fallback = createLuaSkinFoundationCandidateTestConnection(stalled.virtualHostUrl("/first-stalls"), limits,
        {.candidates = {"127.0.0.1", "127.0.0.1"}, .beforeAttempt = [&](auto index) { attempts.push_back(index); }});
    const auto response = fallback->readBody();
    expect(response.body && *response.body == "alpha\r\nbeta\n" && stalled.requests() == 2 &&
           attempts == std::vector<std::size_t>({0, 1}),
           "black-holed first response leaves original deadline budget for another candidate");
  }

  for (bool bodyFailure : {false, true}) {
    attempts.clear();
    limits.deadline = std::chrono::steady_clock::now() + 2s;
    limits.maximumBytes = bodyFailure ? 1 : 1024;
    auto noRetry = createLuaSkinFoundationCandidateTestConnection(
        server.virtualHostUrl(bodyFailure ? "/ok" : "/error"), limits,
        {.candidates = {"127.0.0.1", "127.0.0.1"}, .beforeAttempt = [&](auto index) { attempts.push_back(index); }});
    const auto code = noRetry->responseCode();
    expect(code.code == (bodyFailure ? 200 : 503), "candidate publishes completed HTTP status");
    if (bodyFailure) expect(noRetry->readBody().failure.has_value(), "body failure is reported");
    else expect(noRetry->responseCode().code == 503, "HTTP failure remains the original response");
    expect(attempts == std::vector<std::size_t>({0}), "HTTP status and body failures do not retry another address");
  }
}
#endif

} // namespace

int main() {
  std::string manyLines;
  for (int i = 0; i < 1500; ++i) manyLines += "x\n";
  const auto lines = LuaSkinHttpClient::readLines(manyLines);
  expect(!lines.failure && lines.lines.size() == 1500, "byte-bounded lines have no line count cap");
  expect(!LuaSkinHttpClient::readLines(std::string(65537, 'x')).failure, "default body accepts more than old character cap");
  expect(LuaSkinHttpClient::readLines(std::string(1048577, '\n')).failure.has_value(), "newlines count against raw byte cap");
  testRequestOptions();
#if defined(ASOBMASHOW_TEST_CURL_HTTP_TRANSPORT)
  testStalledDnsReleasesAsyncWorkers();
#endif
#if defined(ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT)
  testFoundationCandidateFallback();
#endif
  testResponseCodeReturnsBeforeAStalledBody();
  testProductionTransportStagesRedirectsAndBoundsTail();
  testProductionTransportHonorsCancellation();
  testStalledBodyCanBeCancelledAfterHeaders();
  if (failures != 0) {
    std::cerr << failures << " Lua skin HTTP transport test(s) failed\n";
    return 1;
  }
  std::cout << "Lua skin HTTP transport tests passed\n";
  return 0;
}
