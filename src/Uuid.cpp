#include "Uuid.h"

#include <array>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>

namespace uuid {

std::string generateV4() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random;
  for (unsigned char &value : bytes) {
    value = static_cast<unsigned char>(random());
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

  std::ostringstream encoded;
  encoded << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      encoded << '-';
    }
    encoded << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  }
  return encoded.str();
}

bool isStructurallyValid(std::string_view value) noexcept {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') {
        return false;
      }
      continue;
    }
    if (std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
      return false;
    }
  }
  return true;
}

bool isCanonicalLowerV4(std::string_view value) noexcept {
  if (!isStructurallyValid(value) || value[14] != '4' ||
      (value[19] != '8' && value[19] != '9' && value[19] != 'a' &&
       value[19] != 'b')) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      continue;
    }
    const char character = value[index];
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

} // namespace uuid
