#include "IrOutboxModels.h"

#include <algorithm>

namespace ir {
namespace {

std::size_t utf8Boundary(std::string_view value, std::size_t maximum) {
  std::size_t length = std::min(value.size(), maximum);
  while (length > 0 && length < value.size() &&
         (static_cast<unsigned char>(value[length]) & 0xc0U) == 0x80U) {
    --length;
  }
  return length;
}

} // namespace

std::string sanitizeDiagnostic(std::string_view value) {
  std::string result(value.substr(0, utf8Boundary(value, kMaximumDiagnosticBytes)));
  for (char &character : result) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if ((byte < 0x20U && character != '\n' && character != '\t') ||
        byte == 0x7fU) {
      character = ' ';
    }
  }
  return result;
}

} // namespace ir
