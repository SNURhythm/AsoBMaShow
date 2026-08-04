#pragma once

#include <mutex>

namespace text_runtime {
bool acquire() noexcept;
void release() noexcept;
std::mutex &mutex() noexcept;
} // namespace text_runtime
