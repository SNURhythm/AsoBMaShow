#pragma once

#include "RAII.h"

extern "C" void stbi_image_free(void *retval_from_stbi_load);

using StbiImageHandle = UniqueResource<unsigned char, stbi_image_free>;
