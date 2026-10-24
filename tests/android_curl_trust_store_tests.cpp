#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "../src/AndroidCurlTrustStore.h"
#include "../src/RAII.h"

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
using TrustSocket = SOCKET;
constexpr TrustSocket invalidTrustSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using TrustSocket = int;
constexpr TrustSocket invalidTrustSocket = -1;
#endif

namespace {
namespace fs = std::filesystem;

void require(bool passed, const char *message) {
  if (!passed) throw std::runtime_error(message);
}

struct Certificate {
  UniqueResource<EVP_PKEY, EVP_PKEY_free> key{nullptr};
  UniqueResource<X509, X509_free> cert{nullptr};
};

Certificate certificate(const char *name, const Certificate *issuer = nullptr) {
  Certificate result;
  result.key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"));
  result.cert.reset(X509_new());
  require(result.key && result.cert, "allocate TLS fixture certificate");
  require(X509_set_version(result.cert.get(), 2) == 1 &&
          ASN1_INTEGER_set(X509_get_serialNumber(result.cert.get()), issuer ? 2 : 1) == 1 &&
          X509_gmtime_adj(X509_getm_notBefore(result.cert.get()), -3600) &&
          X509_gmtime_adj(X509_getm_notAfter(result.cert.get()), 86400) &&
          X509_set_pubkey(result.cert.get(), result.key.get()) == 1,
          "initialize TLS fixture certificate");
  auto *subject = X509_get_subject_name(result.cert.get());
  require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
          reinterpret_cast<const unsigned char *>(name), -1, -1, 0) == 1 &&
          X509_set_issuer_name(result.cert.get(), issuer ? X509_get_subject_name(issuer->cert.get()) : subject) == 1,
          "set TLS fixture certificate names");
  auto extension = [&](int nid, const char *value) {
    UniqueResource<X509_EXTENSION, X509_EXTENSION_free> ext(
        X509V3_EXT_conf_nid(nullptr, nullptr, nid, value));
    require(ext && X509_add_ext(result.cert.get(), ext.get(), -1) == 1, "add TLS fixture extension");
  };
  extension(NID_basic_constraints, issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
  if (issuer) extension(NID_subject_alt_name, "DNS:localhost");
  require(X509_sign(result.cert.get(), issuer ? issuer->key.get() : result.key.get(), EVP_sha256()) > 0,
          "sign TLS fixture certificate");
  return result;
}

std::string pem(const Certificate &certificate) {
  UniqueResource<BIO, BIO_free> output(BIO_new(BIO_s_mem()));
  require(output && PEM_write_bio_X509(output.get(), certificate.cert.get()) == 1, "encode fixture PEM");
  char *data = nullptr;
  const auto size = BIO_get_mem_data(output.get(), &data);
  return {data, static_cast<std::size_t>(size)};
}

std::string hashName(const Certificate &certificate, bool old) {
  const auto *name = X509_get_subject_name(certificate.cert.get());
  std::ostringstream value;
  value << std::hex << std::setw(8) << std::setfill('0')
        << (old ? X509_NAME_hash_old(name) : X509_NAME_hash(name)) << ".0";
  return value.str();
}

void write(const fs::path &path, std::string_view data) {
  std::ofstream file(path, std::ios::binary);
  file.write(data.data(), static_cast<std::streamsize>(data.size()));
  file.close();
  require(static_cast<bool>(file), "write trust-store fixture");
}

struct Directory {
  fs::path path = fs::temp_directory_path() /
      ("asobmashow-curl-trust-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { require(fs::create_directory(path), "create trust-store fixture directory"); }
  ~Directory() { std::error_code error; fs::remove_all(path, error); }
};

void closeSocket(TrustSocket socket) {
#if defined(_WIN32)
  closesocket(socket);
#else
  ::close(socket);
#endif
}

class TlsServer {
public:
  explicit TlsServer(const Certificate &identity) : context_(SSL_CTX_new(TLS_server_method())) {
    require(context_ && SSL_CTX_use_certificate(context_.get(), identity.cert.get()) == 1 &&
            SSL_CTX_use_PrivateKey(context_.get(), identity.key.get()) == 1, "configure local TLS server");
    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener_ != invalidTrustSocket, "create local TLS listener");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    require(::bind(listener_, reinterpret_cast<sockaddr *>(&address), size) == 0 &&
            ::listen(listener_, 4) == 0 &&
            ::getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size) == 0,
            "bind local TLS listener");
    port_ = ntohs(address.sin_port);
    worker_ = std::jthread([this](std::stop_token stop) {
      while (!stop.stop_requested()) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener_, &readable);
        timeval timeout{0, 10000};
        if (::select(static_cast<int>(listener_ + 1), &readable, nullptr, nullptr, &timeout) <= 0) continue;
        const auto client = ::accept(listener_, nullptr, nullptr);
        if (client == invalidTrustSocket) continue;
#if defined(_WIN32)
        DWORD ioTimeout = 1000;
#else
        timeval ioTimeout{1, 0};
#endif
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ioTimeout), sizeof(ioTimeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ioTimeout), sizeof(ioTimeout));
#if defined(__APPLE__)
        int enabled = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
        {
          UniqueResource<SSL, SSL_free> ssl(SSL_new(context_.get()));
          if (ssl && SSL_set_fd(ssl.get(), static_cast<int>(client)) == 1 && SSL_accept(ssl.get()) == 1) {
            std::array<char, 4096> request{};
            if (SSL_read(ssl.get(), request.data(), static_cast<int>(request.size())) > 0) {
              constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
              (void)SSL_write(ssl.get(), response.data(), static_cast<int>(response.size()));
            }
          }
        }
        closeSocket(client);
      }
    });
  }
  ~TlsServer() {
    worker_.request_stop();
    worker_.join();
    closeSocket(listener_);
  }
  unsigned port() const { return port_; }
private:
  UniqueResource<SSL_CTX, SSL_CTX_free> context_;
  TrustSocket listener_ = invalidTrustSocket;
  unsigned port_ = 0;
  std::jthread worker_;
};

CURLcode request(const TlsServer &server, std::string bundle, const fs::path &oldStore,
                  const fs::path &oldRoot, bool wrongHost = false) {
  UniqueResource<CURL, curl_easy_cleanup> curl(curl_easy_init());
  require(curl != nullptr, "allocate native TLS request");
  const auto url = std::string("https://") + (wrongHost ? "127.0.0.1:" : "localhost:") +
                   std::to_string(server.port()) + "/";
  const auto resolve = "localhost:" + std::to_string(server.port()) + ":127.0.0.1";
  UniqueResource<curl_slist, curl_slist_free_all> hosts(curl_slist_append(nullptr, resolve.c_str()));
  require(hosts != nullptr, "configure local hostname resolution");
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_RESOLVE, hosts.get());
  curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 1500L);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
      +[](char *, std::size_t size, std::size_t count, void *) { return size * count; });
  // Seed stale trust and disabled verification. Android setup must replace the
  // former and enable the latter, not merely append new anchors.
  curl_easy_setopt(curl.get(), CURLOPT_CAINFO, oldRoot.string().c_str());
  curl_easy_setopt(curl.get(), CURLOPT_CAPATH, oldStore.string().c_str());
  curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 0L);
  require(android_curl_trust::configure(curl.get(), bundle), "configure device PEM bundle");
  std::fill(bundle.begin(), bundle.end(), 'x'); // the easy handle must own COPY
  return curl_easy_perform(curl.get());
}

void testTrustBundle() {
  Directory fixture;
  const auto apex = fixture.path / "apex";
  const auto system = fixture.path / "system";
  fs::create_directory(apex);
  fs::create_directory(system);
  const auto active = certificate("Android active root");
  const auto obsolete = certificate("Android obsolete root");
  const auto activePem = pem(active), obsoletePem = pem(obsolete);
  require(hashName(active, true) != hashName(active, false), "fixture distinguishes old and modern subject hashes");
  // Actual Android-style old hash name and a textual certificate dump after PEM.
  const auto activePath = apex / hashName(active, true);
  write(activePath, activePem + "Certificate:\n    Android root metadata\n");
  const auto obsoletePath = system / hashName(obsolete, false);
  write(obsoletePath, obsoletePem);
  fs::create_directory(apex / "ignored-directory");
#if !defined(_WIN32)
  fs::create_symlink(obsoletePath, apex / "ignored-symlink.0");
#endif
  auto bundle = android_curl_trust::loadBundle(apex, system);
  require(bundle == activePem, "old-hash PEM becomes a normalized bundle without stale roots or nonregular files");
  require(android_curl_trust::loadBundle(fixture.path / "absent-apex", system) == obsoletePem,
          "system roots are used when APEX is absent");

  const auto activeLeaf = certificate("localhost", &active);
  const auto obsoleteLeaf = certificate("localhost", &obsolete);
  TlsServer activeServer(activeLeaf), obsoleteServer(obsoleteLeaf);
  require(request(activeServer, bundle, system, obsoletePath) == CURLE_OK,
          "native HTTPS trusts normalized old-hash Android root and copies PEM data");
  require(request(activeServer, bundle, system, obsoletePath, true) == CURLE_PEER_FAILED_VERIFICATION,
          "native HTTPS rejects a certificate for the wrong hostname");
  require(request(obsoleteServer, bundle, system, obsoletePath) == CURLE_PEER_FAILED_VERIFICATION,
          "active APEX trust rejects an obsolete system chain despite previously configured CAINFO/CAPATH");

  fs::remove(activePath);
  require(android_curl_trust::loadBundle(apex, system).empty(), "empty active APEX does not fall back to system roots");
  write(activePath, "-----BEGIN CERTIFICATE-----\ntruncated");
  require(android_curl_trust::loadBundle(apex, system).empty(), "truncated PEM fails closed");
  write(activePath, activePem + std::string(65537 - activePem.size(), ' '));
  require(android_curl_trust::loadBundle(apex, system).empty(), "certificate file over 64 KiB fails closed");
  require(android_curl_trust::loadBundle(activePath, system).empty(), "unreadable active store does not fall back");
  UniqueResource<CURL, curl_easy_cleanup> curl(curl_easy_init());
  require(curl && !android_curl_trust::configure(curl.get(), {}), "empty trust bundle refuses request configuration");
  require(!android_curl_trust::configure(nullptr, activePem), "missing easy handle fails closed");

  const auto large = fixture.path / "large";
  fs::create_directory(large);
  const auto padded = activePem + std::string(65536 - activePem.size(), ' ');
  for (int index = 0; index < 64; ++index) write(large / std::to_string(index), padded);
  require(!android_curl_trust::loadBundle(large, system).empty(), "4 MiB source budget accepts its boundary");
  write(large / "overflow", activePem);
  require(android_curl_trust::loadBundle(large, system).empty(), "source data above 4 MiB fails closed");
}
} // namespace

int RunAndroidCurlTrustStoreTests() {
#if defined(SIGPIPE)
  std::signal(SIGPIPE, SIG_IGN);
#endif
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
  int result = 0;
  try {
    testTrustBundle();
    std::cout << "Android curl trust-store tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    result = 1;
  }
  curl_global_cleanup();
  return result;
}
