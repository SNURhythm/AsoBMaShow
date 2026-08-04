#pragma once

#include "BeatorajaSkinConfiguration.h"
#include "SkinCompatibilityDiagnostics.h"

#include <optional>
#include <string>
#include <vector>

namespace skin {

struct SkinHeaderCategory {
  std::string name;
  std::vector<std::string> items;
};

struct SkinHeaderOptionChoice {
  std::string label;
  int value = 0;
};

struct SkinHeaderOption {
  std::string category;
  std::string name;
  std::vector<SkinHeaderOptionChoice> choices;
  std::string defaultLabel;
};

struct SkinHeaderFile {
  std::string category;
  std::string name;
  std::string pattern;
  std::string defaultValue;
};

struct SkinHeaderOffset {
  std::string category;
  std::string name;
  int id = 0;
  OffsetPermissionMask permissions = 0;
};

struct BeatorajaSkinHeader {
  int type = -1;
  int width = 1280;
  int height = 720;
  std::string name;
  std::string author;
  std::vector<SkinHeaderCategory> categories;
  std::vector<SkinHeaderOption> options;
  std::vector<SkinHeaderFile> files;
  std::vector<SkinHeaderOffset> offsets;
};

struct HeaderDecodeResult {
  std::optional<BeatorajaSkinHeader> header;
  std::vector<SkinDiagnostic> diagnostics;
};

struct ConfigurationReconcileResult {
  std::optional<BeatorajaSkinConfiguration> configuration;
  EntryProfileSettings reconciledSettings;
  std::vector<SkinDiagnostic> diagnostics;
};

struct BeatorajaSkinModelDecodeResult;

} // namespace skin
