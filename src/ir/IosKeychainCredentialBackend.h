#pragma once

#include "IrCredentialBackend.h"

#include <memory>

namespace ir {

[[nodiscard]] std::unique_ptr<IrCredentialBackend>
CreateIosKeychainCredentialBackend();

} // namespace ir
