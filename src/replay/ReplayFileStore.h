#pragma once

#include "BeatorajaReplayCodec.h"
#include "BeatorajaReplayPath.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace replay {

enum class ReplayFileState { Available, Missing, Corrupt, Unsafe, IoFailure };

struct ReplayFileMetadata {
  std::filesystem::path relativePath;
  std::string sha256;
  std::uint64_t compressedSize = 0;
  int codecVersion = 1;

  bool operator==(const ReplayFileMetadata &) const = default;
};

struct FinalizeOutcome {
  std::optional<ReplayFileMetadata> metadata;
  bool existingIdenticalFile = false;
  std::string diagnostic;
};

struct ReplayFileInspection {
  ReplayFileState state = ReplayFileState::IoFailure;
  std::optional<ReplayFileMetadata> metadata;
  std::string diagnostic;
};

struct ExpectedReplayIdentity {
  std::vector<std::string> stageSha256;
  bool course = false;
};

struct ReplayFileStoreFaults {
  std::function<bool(std::string_view)> failAt;
};

class ReplayFileStore {
public:
  explicit ReplayFileStore(std::filesystem::path profileRoot,
                           ReplayFileStoreFaults faults = {});

  [[nodiscard]] FinalizeOutcome finalize(const ReplayPathIdentity &identity,
                                         std::span<const std::byte> encoded,
                                         const BeatorajaReplayCodec &codec,
                                         const ExpectedReplayIdentity &expected,
                                         std::string_view attemptToken);

  [[nodiscard]] ReplayFileInspection
  inspect(const ReplayFileMetadata &metadata) const;

  [[nodiscard]] ReplayDecodeOutcome
  load(const ReplayFileMetadata &metadata,
       const BeatorajaReplayCodec &codec) const;

  bool remove(const ReplayFileMetadata &metadata, std::string &diagnostic);

  bool copyToBeatorajaSlot(const ReplayFileMetadata &source,
                           std::string_view stem, int slot,
                           std::string &diagnostic);

  void removeStaleTemporaryFiles(std::chrono::system_clock::time_point cutoff);

private:
  std::filesystem::path profileRoot_;
  ReplayFileStoreFaults faults_;
};

} // namespace replay
