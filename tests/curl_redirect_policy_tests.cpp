#include "../src/CurlRAII.h"

#include <cassert>
#include <string_view>

int main() {
  assert(std::string_view(CurlRedirectProtocolsForInitialUrl(
             "http://legacy.example.test/start")) == "http,https");
  assert(std::string_view(CurlRedirectProtocolsForInitialUrl(
             "HTTP://legacy.example.test/start")) == "http,https");
  assert(std::string_view(CurlRedirectProtocolsForInitialUrl(
             "https://secure.example.test/start")) == "https");
  assert(std::string_view(CurlRedirectProtocolsForInitialUrl(
             "HTTPS://secure.example.test/start")) == "https");
  return 0;
}
