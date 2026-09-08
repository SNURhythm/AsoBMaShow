#pragma once

#include "IrDriver.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ir {

inline constexpr auto kIrRankingCacheTtl = std::chrono::minutes(10);

struct IrRankingServiceOptions {
  using SteadyTimePoint = std::chrono::steady_clock::time_point;

  std::function<SteadyTimePoint()> monotonicNow;
  std::function<std::string(std::string_view profileId,
                            std::string_view providerId)>
      credentialLookup;
};

class IrRankingService {
public:
  IrRankingService(const IrDriverRegistry &drivers, IrHttpClient &http,
                   IrRankingServiceOptions options = {});
  ~IrRankingService();

  IrRankingService(const IrRankingService &) = delete;
  IrRankingService &operator=(const IrRankingService &) = delete;

  [[nodiscard]] std::uint64_t open(IrRankingRequest request);
  [[nodiscard]] std::uint64_t refresh();
  [[nodiscard]] std::uint64_t refresh(IrRankingRequest request);
  [[nodiscard]] bool loadNextPage(std::uint64_t generation);
  void close(std::uint64_t generation);
  void pauseAndCancel();
  [[nodiscard]] IrRankingSnapshot snapshot() const;
  void invalidate(const IrRankingInvalidation &invalidation);
  void activateProfile(std::string_view profileId);
  void stop();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ir
