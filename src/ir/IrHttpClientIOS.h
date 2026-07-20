#pragma once

#include "IrHttpClient.h"

namespace ir {

[[nodiscard]] IrHttpResponse
PerformIrHttpRequestIOS(const IrHttpRequest &request,
                        std::stop_token stopToken) noexcept;

} // namespace ir
