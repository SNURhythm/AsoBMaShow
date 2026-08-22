#include "Utils.h"
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <iterator>
#include <limits>
#include "path.h"
#include "targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "iOSNatives.hpp"
#include <CoreFoundation/CoreFoundation.h>
#elif TARGET_OS_ANDROID
#include "AndroidNatives.h"
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#if !TARGET_OS_ANDROID && !defined(_WIN32) && !defined(__APPLE__) && \
    __has_include(<iconv.h>)
#include <iconv.h>
#define ASOBMASHOW_UTILS_HAS_ICONV 1
#else
#define ASOBMASHOW_UTILS_HAS_ICONV 0
#endif

namespace {
constexpr char32_t kUnicodeReplacementChar = 0xFFFD;

bool isUnicodeScalarValue(char32_t codePoint) {
  return codePoint <= 0x10FFFF &&
         (codePoint < 0xD800 || codePoint > 0xDFFF);
}

void appendUtf8CodePoint(std::string &output, char32_t codePoint) {
  if (!isUnicodeScalarValue(codePoint)) {
    codePoint = kUnicodeReplacementChar;
  }

  if (codePoint <= 0x7F) {
    output.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7FF) {
    output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else if (codePoint <= 0xFFFF) {
    output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else {
    output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  }
}
} // namespace

unsigned int parallel_worker_count(size_t n) {
  if (n == 0) {
    return 0;
  }

  unsigned int hwThreads = std::thread::hardware_concurrency();
  if (hwThreads == 0) {
    hwThreads = 4;
  }

  // Keep headroom for render/audio/main threads to reduce frame-time spikes.
  unsigned int reservedThreads = 1;
  if (hwThreads > 8) {
    reservedThreads = 4;
  } else if (hwThreads > 4) {
    reservedThreads = 2;
  }

  unsigned int workerThreads =
      hwThreads > reservedThreads ? hwThreads - reservedThreads : 1;
  return std::min<unsigned int>(workerThreads, static_cast<unsigned int>(n));
}

void parallel_for(size_t n, std::function<void(int start, int end)> f) {
  const unsigned int workerThreads = parallel_worker_count(n);
  if (workerThreads == 0) {
    return;
  }

  if (workerThreads <= 1) {
    f(0, static_cast<int>(n));
    return;
  }

  const size_t batchSize = (n + workerThreads - 1) / workerThreads;
  std::vector<std::thread> threads;
  threads.reserve(workerThreads);

  for (unsigned int i = 0; i < workerThreads; ++i) {
    const size_t start = static_cast<size_t>(i) * batchSize;
    if (start >= n) {
      break;
    }
    const size_t end = std::min(n, start + batchSize);
    threads.emplace_back([&f, start, end]() {
      f(static_cast<int>(start), static_cast<int>(end));
    });
  }

  for (auto &t : threads) {
    t.join();
  }
}

std::optional<std::string> cp932_to_utf8(std::string_view value) {
  if (value.empty()) {
    return std::string{};
  }
#if TARGET_OS_ANDROID
  return ConvertAndroidMs932ToUtf8(value);
#elif defined(_WIN32)
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  const int inputLength = static_cast<int>(value.size());
  const int wideLength = MultiByteToWideChar(932, 0, value.data(), inputLength,
                                              nullptr, 0);
  if (wideLength <= 0) {
    return std::nullopt;
  }
  std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
  if (MultiByteToWideChar(932, 0, value.data(), inputLength, wide.data(),
                          wideLength) != wideLength) {
    return std::nullopt;
  }
  const int utf8Length = WideCharToMultiByte(
      CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
  if (utf8Length <= 0) {
    return std::nullopt;
  }
  std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, utf8.data(),
                          utf8Length, nullptr, nullptr) != utf8Length) {
    return std::nullopt;
  }
  return utf8;
#elif defined(__APPLE__)
  const CFStringRef decoded = CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(value.data()),
      static_cast<CFIndex>(value.size()), kCFStringEncodingDOSJapanese, false);
  if (decoded == nullptr) {
    return std::nullopt;
  }
  const CFIndex maximumBytes =
      CFStringGetMaximumSizeForEncoding(CFStringGetLength(decoded),
                                        kCFStringEncodingUTF8) +
      1;
  std::string utf8(static_cast<std::size_t>(maximumBytes), '\0');
  const bool converted = CFStringGetCString(
      decoded, utf8.data(), maximumBytes, kCFStringEncodingUTF8);
  CFRelease(decoded);
  if (!converted) {
    return std::nullopt;
  }
  utf8.resize(std::strlen(utf8.c_str()));
  return utf8;
#elif ASOBMASHOW_UTILS_HAS_ICONV
  iconv_t converter = iconv_open("UTF-8", "CP932");
  if (converter == reinterpret_cast<iconv_t>(-1)) {
    return std::nullopt;
  }
  std::string output(std::max<std::size_t>(value.size() * 4U, 32U), '\0');
  char *input = const_cast<char *>(value.data());
  std::size_t inputBytes = value.size();
  std::size_t outputOffset = 0;
  while (inputBytes > 0) {
    char *nextOutput = output.data() + outputOffset;
    std::size_t outputBytes = output.size() - outputOffset;
    const std::size_t status =
        iconv(converter, &input, &inputBytes, &nextOutput, &outputBytes);
    outputOffset = output.size() - outputBytes;
    if (status != static_cast<std::size_t>(-1)) {
      continue;
    }
    if (errno != E2BIG || output.size() >
                              std::numeric_limits<std::size_t>::max() / 2U) {
      iconv_close(converter);
      return std::nullopt;
    }
    output.resize(output.size() * 2U);
  }
  iconv_close(converter);
  output.resize(outputOffset);
  return output;
#else
  return std::nullopt;
#endif
}

std::string ws2s_utf8(const std::wstring &wstr) {
  std::string output;
  output.reserve(wstr.size());
  for (std::size_t i = 0; i < wstr.size(); ++i) {
    char32_t codePoint = static_cast<char32_t>(wstr[i]);
    if constexpr (sizeof(wchar_t) == 2) {
      if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
        if (i + 1 < wstr.size()) {
          const char32_t low = static_cast<char32_t>(wstr[i + 1]);
          if (low >= 0xDC00 && low <= 0xDFFF) {
            codePoint =
                0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
            ++i;
          } else {
            codePoint = kUnicodeReplacementChar;
          }
        } else {
          codePoint = kUnicodeReplacementChar;
        }
      } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
        codePoint = kUnicodeReplacementChar;
      }
    }
    appendUtf8CodePoint(output, codePoint);
  }
  return output;
}

std::filesystem::path
Utils::GetDocumentsPath(const std::filesystem::path &SubPath) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return GetIOSDocumentsPath() / SubPath;
#elif TARGET_OS_ANDROID
  return GetAndroidExternalFilesDir() / SubPath;
#else
#ifdef _WIN32
  static std::wstring WindowsUserDir;
  if (WindowsUserDir.empty()) {
    wchar_t *UserPath;

    // get the My Documents directory
    HRESULT Ret =
        SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &UserPath);
    if (SUCCEEDED(Ret)) {
      // make the base user dir path
      WindowsUserDir = std::wstring(UserPath);
      CoTaskMemFree(UserPath);
    }
  }
  return std::filesystem::path(WindowsUserDir) / GameName / SubPath;
#else
  if (const char *home = std::getenv("HOME");
      home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / GameName / SubPath;
  }
  std::error_code currentPathError;
  const std::filesystem::path currentPath =
      std::filesystem::current_path(currentPathError);
  if (!currentPathError && !currentPath.empty()) {
    return currentPath / GameName / SubPath;
  }
  return std::filesystem::path(GameName) / SubPath;
#endif
#endif
}

std::filesystem::path Utils::GetStoragePathRelativeToDocuments(
    const std::filesystem::path &Path, const std::filesystem::path &SubPath) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const std::filesystem::path documents =
      GetDocumentsPath(SubPath).lexically_normal();
  const std::filesystem::path normalizedPath = Path.lexically_normal();
  const std::filesystem::path relative =
      normalizedPath.lexically_relative(documents);
  if (relative.empty()) {
    return Path;
  }

  const auto firstComponent = relative.begin();
  if (firstComponent == relative.end() ||
      *firstComponent == std::filesystem::path("..")) {
    return Path;
  }
  if (*firstComponent == std::filesystem::path(".") &&
      std::next(firstComponent) == relative.end()) {
    return {};
  }
  return relative;
#else
  (void)SubPath;
  return Path;
#endif
}

std::string Utils::GetStoragePathUtf8RelativeToDocuments(
    const std::filesystem::path &Path, const std::filesystem::path &SubPath) {
  return path_t_to_utf8(
      fspath_to_path_t(GetStoragePathRelativeToDocuments(Path, SubPath)));
}

bool Utils::EnsureDirectoryExists(const std::filesystem::path &Path,
                                  std::error_code &Error) {
  Error.clear();
  if (Path.empty()) {
    Error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  std::filesystem::create_directories(Path, Error);
  return !Error;
}

threadRAII::threadRAII(std::thread &&_th) { th = std::move(_th); }
threadRAII::~threadRAII() {
  if (th.joinable()) {
    th.join();
  }
}
