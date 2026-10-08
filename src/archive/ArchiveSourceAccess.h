#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace archive_source {

// The readable path may be descriptor-backed. Only the caller's logical path
// belongs in database records, virtual member paths, or persistent cache keys.
struct Access {
  std::filesystem::path path;
  std::shared_ptr<void> owner;
  std::string error;
  explicit operator bool() const noexcept { return !path.empty() && error.empty(); }
};

inline bool isReference(const std::filesystem::path &path) {
  return !path.empty() && *path.begin() == "@androidarchive@";
}

inline bool validReference(const std::filesystem::path &path) {
  if (!isReference(path)) return false;
  std::size_t count = 0;
  for (const auto &part : path) {
    if (part.empty() || part == "." || part == "..") return false;
    ++count;
  }
  return count == 3;
}

using Resolver = std::function<Access(const std::filesystem::path &)>;
inline std::mutex resolverMutex;
inline Resolver sourceResolver;

inline void setResolver(Resolver resolver) {
  std::lock_guard lock(resolverMutex);
  sourceResolver = std::move(resolver);
}

inline Access resolve(const std::filesystem::path &path) {
  if (!isReference(path)) return {path, {}, {}};
  if (!validReference(path)) return {{}, {}, "Invalid archive reference."};
  Resolver resolver;
  {
    std::lock_guard lock(resolverMutex);
    resolver = sourceResolver;
  }
  return resolver ? resolver(path)
                  : Access{{}, {}, "Archive reference is unavailable."};
}

inline bool fileState(const std::filesystem::path &path, std::uintmax_t &size,
                      std::filesystem::file_time_type &mtime) {
  const auto source = resolve(path);
  if (!source) return false;
  std::error_code error;
  if (!std::filesystem::is_regular_file(source.path, error) || error) return false;
  size = std::filesystem::file_size(source.path, error);
  if (error) return false;
  mtime = std::filesystem::last_write_time(source.path, error);
  return !error;
}

// Retaining the lease also covers SDK readers that hold this stream for later
// seeks. Every stream opens its own cursor; duplicating an fd would share it.
class InputFile : public std::ifstream {
public:
  InputFile() = default;
  explicit InputFile(const std::filesystem::path &path,
                     std::ios::openmode mode = std::ios::in) { open(path, mode); }
  void open(const std::filesystem::path &path,
            std::ios::openmode mode = std::ios::in) {
    source_ = resolve(path);
    if (source_) std::ifstream::open(source_.path, mode);
    else setstate(std::ios::failbit);
  }
private:
  Access source_;
};

} // namespace archive_source
