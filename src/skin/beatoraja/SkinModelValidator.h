#pragma once

#include "BeatorajaSkinModel.h"

namespace skin {

class SkinModelValidator final {
public:
  SkinModelValidationResult validate(BeatorajaSkinModel) const;
};

} // namespace skin
