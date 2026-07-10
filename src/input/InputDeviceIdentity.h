#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct SdlDeviceIdentityDescriptor {
  std::string guid;
  std::string serial;
  std::string path;
  std::string name;
};

class InputDeviceIdentity {
public:
  std::string connect(const SdlDeviceIdentityDescriptor &descriptor);
  void disconnect(std::string_view stableId);

private:
  struct NameSlot {
    std::string base;
    std::size_t index = 0;
  };

  std::unordered_map<std::string, std::vector<bool>> nameSlots_;
  std::unordered_map<std::string, NameSlot> assignedNameSlots_;
};
