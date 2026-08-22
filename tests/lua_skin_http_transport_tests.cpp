#include "skin/beatoraja/LuaSkinCurlHttpTransport.h"
#include "skin/beatoraja/LuaSkinHttpClient.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
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
    if (path == "/redirect") {
      (void)sendAll(client,
                    "HTTP/1.1 302 Found\r\nLocation: /ok\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n");
      return;
    }
    if (path == "/overflow") {
      const std::string body(65537, 'a');
      const std::string header =
          "HTTP/1.1 200 OK\r\nContent-Length: " +
          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
      (void)sendAll(client, header);
      (void)sendAll(client, body);
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
      stalledRequests_.fetch_add(1);
      if (!sendAll(client,
                   "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"
                   "Connection: close\r\n\r\n")) {
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
      respond(client, path);
      closeSocket(client);
  }

  Socket listener_ = invalidSocket;
  std::uint16_t port_ = 0;
  std::jthread thread_;
  std::vector<std::jthread> workers_;
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
         "production GET rejects the first UTF-16 character over the bound");

  const auto tail = client.get(server.url("/tail"), 1000);
  const auto tailLines = tail.response
                             ? LuaSkinHttpClient::readLines(tail.response->body)
                             : LuaSkinHttpLinesResult{};
  expect(tail.response && !tailLines.failure && tailLines.lines.size() == 1024,
         "production GET stops after the pinned line count");
  std::this_thread::sleep_for(20ms);
  expect(server.tailBytesSent() < 256 * 1024,
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

} // namespace

int main() {
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
