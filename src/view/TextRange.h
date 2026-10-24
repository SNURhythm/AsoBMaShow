#pragma once
#include <cstddef>

namespace text_range {
// Half-open UTF-8 byte range. Endpoints must fall on code point boundaries.
struct Range {
  std::size_t begin = 0;
  std::size_t end = 0;
};
}
