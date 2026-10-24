#pragma once

#if defined(ASOBMASHOW_TEST_CURL_HTTP_TRANSPORT)
#include <string>

namespace skin {

// Only the native curl test executable exposes this override. Each connection
// snapshots it under a mutex; it never changes HTTP destination validation.
void setLuaSkinCurlDnsServersForTesting(std::string servers);

} // namespace skin
#endif
