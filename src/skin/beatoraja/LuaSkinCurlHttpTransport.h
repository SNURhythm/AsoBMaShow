#pragma once

#include "LuaSkinHttpClient.h"

#include <memory>
#include <stop_token>

namespace skin {

[[nodiscard]] std::unique_ptr<LuaSkinHttpTransport>
createLuaSkinProductionHttpTransport(std::stop_token stop = {});

} // namespace skin
