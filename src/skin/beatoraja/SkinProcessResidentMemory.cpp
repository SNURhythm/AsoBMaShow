#include "SkinProcessResidentMemory.h"

#include <limits>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace skin {
namespace {

ProcessResidentMemoryNativeQueryResult nativeQuery() noexcept {
#if defined(__APPLE__)
  static_assert(std::numeric_limits<mach_vm_size_t>::digits <=
                std::numeric_limits<std::uint64_t>::digits);
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  const kern_return_t status =
      task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count);
  return {.succeeded = status == KERN_SUCCESS,
          .complete = count >= MACH_TASK_BASIC_INFO_COUNT,
          .residentBytes = static_cast<std::uint64_t>(info.resident_size)};
#else
  return {};
#endif
}

} // namespace

std::optional<std::uint64_t>
currentProcessResidentBytes(ProcessResidentMemoryNativeQuery query) noexcept {
  if (query == nullptr) {
    return std::nullopt;
  }
  const ProcessResidentMemoryNativeQueryResult result = query();
  if (!result.succeeded || !result.complete) {
    return std::nullopt;
  }
  return result.residentBytes;
}

std::optional<std::uint64_t> currentProcessResidentBytes() noexcept {
  return currentProcessResidentBytes(nativeQuery);
}

} // namespace skin
