#pragma once

#include <filesystem>
#include <system_error>

namespace test_support {

// Restore permissions only within an owned test tree, without following links.
// Attempt removal even if restoring permissions failed, and report any failure.
inline std::error_code removeReadOnlyTree(const std::filesystem::path &root) {
  namespace fs = std::filesystem;
  std::error_code error;
  const auto makeWritable = [&error](const fs::path &path) {
    const auto status = fs::symlink_status(path, error);
    if (error) return;
    if (fs::is_directory(status)) {
      fs::permissions(path, fs::perms::owner_all,
                      fs::perm_options::add | fs::perm_options::nofollow, error);
    }
#ifdef _WIN32
    else if (fs::is_regular_file(status)) {
      fs::permissions(path, fs::perms::owner_write,
                      fs::perm_options::add | fs::perm_options::nofollow, error);
    }
#endif
  };
  const auto rootStatus = fs::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory) error.clear();
  if (fs::is_directory(rootStatus) && !error) {
    makeWritable(root);
    if (!error) {
      for (fs::recursive_directory_iterator iterator(root, error), end;
           !error && iterator != end; iterator.increment(error)) {
        makeWritable(iterator->path());
        if (error) break;
      }
    }
  }
  const auto permissionError = error;
  fs::remove_all(root, error);
  return error ? error : permissionError;
}

} // namespace test_support
