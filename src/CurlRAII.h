#pragma once

#include "RAII.h"

#include <curl/curl.h>

using CurlEasyHandle = UniqueResource<CURL, curl_easy_cleanup>;
