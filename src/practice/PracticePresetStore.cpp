#include "PracticePresetStore.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <limits>
#include <random>
#include <ranges>

namespace practice {
namespace {
using Json = nlohmann::json;

std::optional<std::string> normalizedSha256(std::string_view value) {
  if (value.size() != 64 ||
      !std::ranges::all_of(value, [](unsigned char character) {
        return std::isxdigit(character) != 0;
      })) {
    return std::nullopt;
  }
  std::string normalized(value);
  std::ranges::transform(
      normalized, normalized.begin(),
      [](unsigned char character) { return std::tolower(character); });
  return normalized;
}

std::optional<std::string> normalizedName(std::string value) {
  const auto notSpace = [](unsigned char character) {
    return std::isspace(character) == 0;
  };
  const auto first = std::ranges::find_if(value, notSpace);
  const auto last =
      std::ranges::find_if(value | std::views::reverse, notSpace).base();
  if (first >= last) {
    return std::nullopt;
  }
  std::string normalized(first, last);
  if (normalized.size() > 128 || normalized.find('\0') != std::string::npos) {
    return std::nullopt;
  }
  return normalized;
}

template <std::size_t Size>
bool hasOnlyKeys(const Json &document,
                 const std::array<std::string_view, Size> &allowed) {
  if (!document.is_object()) {
    return false;
  }
  return std::ranges::all_of(document.items(), [&](const auto &item) {
    return std::ranges::find(allowed, item.key()) != allowed.end();
  });
}

template <std::size_t Size>
bool hasAllKeys(const Json &document,
                const std::array<std::string_view, Size> &required) {
  return std::ranges::all_of(required, [&](std::string_view key) {
    return document.contains(std::string(key));
  });
}

bool validPresetId(std::string_view id) {
  return id.size() == 32 &&
         std::ranges::all_of(id, [](unsigned char character) {
           return std::isdigit(character) ||
                  (character >= 'a' && character <= 'f');
         });
}

bool strictConfigurationShape(const Json &document) {
  static constexpr std::array<std::string_view, 10> allowed = {
      "chartSha256",  "startMicros", "endMicros",      "loop",
      "countInBeats", "gaugeType",   "gaugeAutoShift", "startingGaugePercent",
      "judge",        "playback"};
  static constexpr std::array<std::string_view, 9> required = {
      "chartSha256",
      "startMicros",
      "endMicros",
      "loop",
      "countInBeats",
      "gaugeType",
      "startingGaugePercent",
      "judge",
      "playback"};
  static constexpr std::array<std::string_view, 2> judgeKeys = {"kind",
                                                                "scalePercent"};
  static constexpr std::array<std::string_view, 2> playbackKeys = {"percent",
                                                                   "mode"};
  if (!hasOnlyKeys(document, allowed) || !hasAllKeys(document, required) ||
      !document.at("judge").is_object() ||
      document.at("judge").size() != judgeKeys.size() ||
      !hasAllKeys(document.at("judge"), judgeKeys) ||
      !hasOnlyKeys(document.at("judge"), judgeKeys) ||
      !document.at("playback").is_object() ||
      document.at("playback").size() != playbackKeys.size() ||
      !hasAllKeys(document.at("playback"), playbackKeys) ||
      !hasOnlyKeys(document.at("playback"), playbackKeys)) {
    return false;
  }
  return document.size() == allowed.size() ||
         (document.size() + 1 == allowed.size() &&
          !document.contains("gaugeAutoShift"));
}

bool strictPortableDocumentShape(const Json &document) {
  static constexpr std::array<std::string_view, 4> rootKeys = {
      "schemaVersion", "chartSha256", "lastUsed", "named"};
  static constexpr std::array<std::string_view, 3> namedKeys = {
      "id", "name", "configuration"};
  if (document.size() != rootKeys.size() || !hasAllKeys(document, rootKeys) ||
      !hasOnlyKeys(document, rootKeys) ||
      !strictConfigurationShape(document.at("lastUsed")) ||
      !document.at("named").is_array()) {
    return false;
  }
  std::vector<std::string> ids;
  for (const Json &encoded : document.at("named")) {
    if (encoded.size() != namedKeys.size() || !hasAllKeys(encoded, namedKeys) ||
        !hasOnlyKeys(encoded, namedKeys) || !encoded.at("id").is_string() ||
        !encoded.at("name").is_string() ||
        !strictConfigurationShape(encoded.at("configuration"))) {
      return false;
    }
    const std::string id = encoded.at("id").get<std::string>();
    const std::string name = encoded.at("name").get<std::string>();
    const auto normalized = normalizedName(name);
    if (!validPresetId(id) || !normalized ||
        std::ranges::find(ids, id) != ids.end()) {
      return false;
    }
    ids.push_back(id);
  }
  return true;
}

const char *gaugeTypeName(GaugeType value) {
  switch (value) {
  case GaugeType::AssistedEasy:
    return "assisted_easy";
  case GaugeType::Easy:
    return "easy";
  case GaugeType::Normal:
    return "normal";
  case GaugeType::Hard:
    return "hard";
  case GaugeType::ExHard:
    return "exhard";
  }
  return "normal";
}

std::optional<GaugeType> gaugeTypeFromName(std::string_view value) {
  if (value == "assisted_easy") {
    return GaugeType::AssistedEasy;
  }
  if (value == "easy") {
    return GaugeType::Easy;
  }
  if (value == "normal") {
    return GaugeType::Normal;
  }
  if (value == "hard") {
    return GaugeType::Hard;
  }
  if (value == "exhard") {
    return GaugeType::ExHard;
  }
  return std::nullopt;
}

Json configurationJson(const Configuration &value) {
  Json result{{"chartSha256", value.chartSha256},
              {"startMicros", value.startMicros},
              {"endMicros", value.endMicros},
              {"loop", value.loop},
              {"countInBeats", value.countInBeats},
              {"gaugeType", gaugeTypeName(value.gaugeType)},
              {"gaugeAutoShift", value.gaugeAutoShift},
              {"judge",
               {{"kind", static_cast<int>(value.judge.kind)},
                {"scalePercent", value.judge.scalePercent}}},
              {"playback",
               {{"percent", value.playback.percent},
                {"mode", static_cast<int>(value.playback.mode)}}}};
  if (value.startingGaugePercent) {
    result["startingGaugePercent"] = *value.startingGaugePercent;
  } else {
    result["startingGaugePercent"] = nullptr;
  }
  return result;
}

std::optional<Configuration> configurationFromJson(const Json &document,
                                                   std::string &error) {
  try {
    if (!document.is_object()) {
      error = "practice configuration must be an object";
      return std::nullopt;
    }
    const auto gauge =
        gaugeTypeFromName(document.at("gaugeType").get<std::string>());
    if (!gauge) {
      error = "practice configuration contains an unknown gauge type";
      return std::nullopt;
    }
    Configuration value;
    value.chartSha256 = document.at("chartSha256").get<std::string>();
    value.startMicros = document.at("startMicros").get<long long>();
    value.endMicros = document.at("endMicros").get<long long>();
    value.loop = document.at("loop").get<bool>();
    value.countInBeats = document.at("countInBeats").get<int>();
    value.gaugeType = *gauge;
    value.gaugeAutoShift = document.value("gaugeAutoShift", false);
    if (!document.at("startingGaugePercent").is_null()) {
      value.startingGaugePercent =
          document.at("startingGaugePercent").get<int>();
    }
    value.judge.kind = static_cast<JudgeOverrideKind>(
        document.at("judge").at("kind").get<int>());
    value.judge.scalePercent =
        document.at("judge").at("scalePercent").get<int>();
    value.playback.percent = document.at("playback").at("percent").get<int>();
    value.playback.mode = static_cast<audio::PlaybackMode>(
        document.at("playback").at("mode").get<int>());
    return value;
  } catch (const Json::exception &exception) {
    error =
        std::string("practice configuration is invalid: ") + exception.what();
    return std::nullopt;
  }
}

PresetData neutralData(const std::string &hash, long long chartEndMicros) {
  PresetData data;
  data.lastUsed.chartSha256 = hash;
  data.lastUsed.startMicros = 0;
  data.lastUsed.endMicros = std::max(0LL, chartEndMicros);
  return data;
}

std::filesystem::path pathFor(const std::filesystem::path &directory,
                              const std::string &hash) {
  return directory / (hash + ".json");
}

PresetLoadResult loadData(const std::filesystem::path &directory,
                          std::string_view chartSha256,
                          long long chartEndMicros) {
  const auto hash = normalizedSha256(chartSha256);
  if (!hash) {
    PresetLoadResult result;
    result.status = versioned_json::LoadStatus::InvalidRoot;
    result.diagnostics.emplace_back("chart SHA-256 must contain 64 hex digits");
    return result;
  }
  PresetLoadResult result{.data = neutralData(*hash, chartEndMicros)};
  const std::array<versioned_json::Migration, 1> migrations = {
      [](Json &, std::string &) { return true; }};
  auto loaded = versioned_json::loadAndMigrate(
      pathFor(directory, *hash), kPresetSchemaVersion, migrations);
  result.status = loaded.status;
  result.diagnostics = std::move(loaded.diagnostics);
  if (loaded.status == versioned_json::LoadStatus::Missing) {
    result.diagnostics.emplace_back(
        "no saved practice data; neutral defaults were selected");
    return result;
  }
  if (loaded.status != versioned_json::LoadStatus::Loaded) {
    return result;
  }

  try {
    if (loaded.document.size() != 4 ||
        loaded.document.at("chartSha256").get<std::string>() != *hash ||
        !loaded.document.at("named").is_array()) {
      result.status = versioned_json::LoadStatus::InvalidRoot;
      result.diagnostics.emplace_back(
          "practice preset file does not match the requested chart");
      return result;
    }
    std::string parseError;
    auto lastUsed =
        configurationFromJson(loaded.document.at("lastUsed"), parseError);
    if (!lastUsed || normalizedSha256(lastUsed->chartSha256) != hash) {
      result.status = versioned_json::LoadStatus::InvalidRoot;
      result.diagnostics.push_back(parseError.empty()
                                       ? "last-used chart SHA-256 mismatches"
                                       : std::move(parseError));
      return result;
    }
    auto sanitizedLastUsed = sanitize(std::move(*lastUsed), chartEndMicros);
    result.data.lastUsed = std::move(sanitizedLastUsed.configuration);
    result.diagnostics.insert(result.diagnostics.end(),
                              sanitizedLastUsed.diagnostics.begin(),
                              sanitizedLastUsed.diagnostics.end());
    result.data.named.clear();
    for (const auto &encoded : loaded.document.at("named")) {
      NamedPreset preset;
      preset.id = encoded.at("id").get<std::string>();
      preset.name = encoded.at("name").get<std::string>();
      auto configuration =
          configurationFromJson(encoded.at("configuration"), parseError);
      if (preset.id.empty() || !normalizedName(preset.name) || !configuration ||
          normalizedSha256(configuration->chartSha256) != hash) {
        result.status = versioned_json::LoadStatus::InvalidRoot;
        result.data = neutralData(*hash, chartEndMicros);
        result.diagnostics.emplace_back(parseError.empty()
                                            ? "named practice preset is invalid"
                                            : std::move(parseError));
        return result;
      }
      auto sanitizedPreset =
          sanitize(std::move(*configuration), chartEndMicros);
      preset.configuration = std::move(sanitizedPreset.configuration);
      result.diagnostics.insert(result.diagnostics.end(),
                                sanitizedPreset.diagnostics.begin(),
                                sanitizedPreset.diagnostics.end());
      result.data.named.push_back(std::move(preset));
    }
  } catch (const Json::exception &exception) {
    result.status = versioned_json::LoadStatus::InvalidRoot;
    result.data = neutralData(*hash, chartEndMicros);
    result.diagnostics.push_back(
        std::string("practice preset file is invalid: ") + exception.what());
  }
  return result;
}

Json presetDataJson(const std::string &hash, const PresetData &data) {
  Json named = Json::array();
  for (const auto &preset : data.named) {
    named.push_back(
        {{"id", preset.id},
         {"name", preset.name},
         {"configuration", configurationJson(preset.configuration)}});
  }
  return {{"schemaVersion", kPresetSchemaVersion},
          {"chartSha256", hash},
          {"lastUsed", configurationJson(data.lastUsed)},
          {"named", std::move(named)}};
}

bool configurationMatchesHash(const Configuration &configuration,
                              const std::string &hash, std::string &error) {
  if (normalizedSha256(configuration.chartSha256) !=
      std::optional<std::string>(hash)) {
    error = "practice configuration chart SHA-256 does not match the store key";
    return false;
  }
  return true;
}

bool loadForMutation(const std::filesystem::path &directory,
                     const std::string &hash, PresetData &data,
                     std::string &error,
                     const Configuration *missingLastUsed = nullptr) {
  auto loaded =
      loadData(directory, hash,
               missingLastUsed == nullptr
                   ? std::numeric_limits<long long>::max()
                   : missingLastUsed->endMicros);
  if (loaded.status != versioned_json::LoadStatus::Loaded &&
      loaded.status != versioned_json::LoadStatus::Missing) {
    error = loaded.diagnostics.empty() ? "unable to load practice preset file"
                                       : loaded.diagnostics.front();
    return false;
  }
  data = std::move(loaded.data);
  if (loaded.status == versioned_json::LoadStatus::Missing &&
      missingLastUsed != nullptr) {
    data.lastUsed = *missingLastUsed;
    data.lastUsed.chartSha256 = hash;
  }
  return true;
}

void normalizeConfigurationHash(Configuration &configuration,
                                const std::string &hash) {
  configuration.chartSha256 = hash;
}

std::string generatePresetId(const PresetData &data) {
  static std::atomic<unsigned long long> sequence{0};
  std::mt19937_64 random(
      static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()) ^
      sequence.fetch_add(1));
  constexpr char hex[] = "0123456789abcdef";
  for (;;) {
    std::string id(32, '0');
    for (char &character : id) {
      character = hex[random() & 0xfU];
    }
    if (std::ranges::none_of(data.named, [&](const NamedPreset &preset) {
          return preset.id == id;
        })) {
      return id;
    }
  }
}
} // namespace

PresetFileKind classifyPresetFilename(std::string_view filename) noexcept {
  constexpr std::string_view primarySuffix = ".json";
  constexpr std::array<std::string_view, 4> sidecarSuffixes = {
      ".tmp", ".bak", ".bak.pending", ".bak.previous"};
  std::string_view primary = filename;
  bool sidecar = false;
  for (const std::string_view suffix : sidecarSuffixes) {
    if (filename.ends_with(suffix)) {
      primary.remove_suffix(suffix.size());
      sidecar = true;
      break;
    }
  }
  if (primary.size() != 64 + primarySuffix.size() ||
      !primary.ends_with(primarySuffix)) {
    return PresetFileKind::Invalid;
  }
  for (const unsigned char character : primary.substr(0, 64)) {
    if (!std::isdigit(character) && !(character >= 'a' && character <= 'f')) {
      return PresetFileKind::Invalid;
    }
  }
  return sidecar ? PresetFileKind::AtomicSidecar : PresetFileKind::Primary;
}

PresetFileValidationResult validatePresetFile(const std::filesystem::path &path,
                                              int expectedSchemaVersion) {
  PresetFileValidationResult result;
  if (classifyPresetFilename(path.filename().string()) !=
      PresetFileKind::Primary) {
    result.diagnostics.emplace_back(
        "practice preset filename is not a normalized chart SHA-256 JSON");
    return result;
  }
  if (expectedSchemaVersion > kPresetSchemaVersion) {
    result.status = versioned_json::LoadStatus::FutureVersion;
    result.diagnostics.emplace_back(
        "practice preset schema is newer than supported");
    return result;
  }
  if (expectedSchemaVersion < 0) {
    result.diagnostics.emplace_back("practice preset schema is invalid");
    return result;
  }

  Json document;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.status = versioned_json::LoadStatus::IoError;
    result.diagnostics.emplace_back("unable to open practice preset file");
    return result;
  }
  try {
    input >> document;
  } catch (const Json::exception &exception) {
    result.status = versioned_json::LoadStatus::Malformed;
    result.diagnostics.push_back(
        std::string("malformed practice preset JSON: ") + exception.what());
    return result;
  }
  if (input.bad()) {
    result.status = versioned_json::LoadStatus::IoError;
    result.diagnostics.emplace_back("I/O failure reading practice preset file");
    return result;
  }
  if (!document.is_object() || !document.contains("schemaVersion") ||
      !document.at("schemaVersion").is_number_integer()) {
    result.diagnostics.emplace_back(
        "practice preset schemaVersion must be an integer");
    return result;
  }
  std::int64_t encodedVersion = 0;
  try {
    if (document.at("schemaVersion").is_number_unsigned()) {
      const auto encoded = document.at("schemaVersion").get<std::uint64_t>();
      if (encoded > static_cast<std::uint64_t>(kPresetSchemaVersion)) {
        result.status = versioned_json::LoadStatus::FutureVersion;
        result.diagnostics.emplace_back(
            "practice preset schema is newer than supported");
        return result;
      }
      encodedVersion = static_cast<std::int64_t>(encoded);
    } else {
      encodedVersion = document.at("schemaVersion").get<std::int64_t>();
    }
  } catch (const Json::exception &exception) {
    result.diagnostics.push_back(
        std::string("practice preset schemaVersion is invalid: ") +
        exception.what());
    return result;
  }
  if (encodedVersion > kPresetSchemaVersion) {
    result.status = versioned_json::LoadStatus::FutureVersion;
    result.diagnostics.emplace_back(
        "practice preset schema is newer than supported");
    return result;
  }
  if (encodedVersion != expectedSchemaVersion) {
    result.diagnostics.emplace_back(
        "practice preset schema does not match the profile manifest");
    return result;
  }
  if (!strictPortableDocumentShape(document)) {
    result.diagnostics.emplace_back(
        "practice preset contains undeclared or invalid schema-v1 fields");
    return result;
  }

  const std::string filename = path.filename().string();
  const std::string hash = filename.substr(0, 64);
  PresetLoadResult loaded =
      loadData(path.parent_path(), hash, std::numeric_limits<long long>::max());
  result.status = loaded.status;
  result.diagnostics = std::move(loaded.diagnostics);
  if (result.status == versioned_json::LoadStatus::Loaded &&
      !result.diagnostics.empty()) {
    result.status = versioned_json::LoadStatus::InvalidRoot;
  }
  return result;
}

bool PresetLoadResult::usable() const noexcept {
  return status == versioned_json::LoadStatus::Loaded ||
         status == versioned_json::LoadStatus::Missing;
}

std::optional<std::string> PresetLoadResult::notice() const {
  if (status == versioned_json::LoadStatus::Missing) {
    return std::nullopt;
  }
  if (!diagnostics.empty()) {
    return diagnostics.front();
  }
  switch (status) {
  case versioned_json::LoadStatus::Loaded:
  case versioned_json::LoadStatus::Missing:
    return std::nullopt;
  case versioned_json::LoadStatus::IoError:
    return "Unable to read practice presets.";
  case versioned_json::LoadStatus::Malformed:
    return "Practice preset JSON is malformed.";
  case versioned_json::LoadStatus::InvalidRoot:
    return "Practice preset data is invalid.";
  case versioned_json::LoadStatus::FutureVersion:
    return "Practice presets were created by a newer version.";
  case versioned_json::LoadStatus::MigrationFailed:
    return "Practice preset migration failed.";
  }
  return "Unable to load practice presets.";
}

PresetStore::PresetStore(std::filesystem::path practiceDirectory,
                         const atomic_file::Operations *operations)
    : practiceDirectory_(std::move(practiceDirectory)),
      operations_(operations) {}

PresetLoadResult PresetStore::load(std::string_view chartSha256,
                                   long long chartEndMicros) const {
  return loadData(practiceDirectory_, chartSha256, chartEndMicros);
}

bool installPresetLoadState(PresetLoadResult loaded, bool applyLastUsed,
                            Configuration &configuration,
                            std::vector<NamedPreset> &namedPresets,
                            std::optional<std::string> &selectedPresetId) {
  if (!loaded.usable()) {
    configuration = std::move(loaded.data.lastUsed);
    namedPresets.clear();
    selectedPresetId.reset();
    return false;
  }
  if (applyLastUsed) {
    configuration = std::move(loaded.data.lastUsed);
    selectedPresetId.reset();
  }
  namedPresets = std::move(loaded.data.named);
  return true;
}

bool PresetStore::saveLastUsed(std::string_view chartSha256,
                               const Configuration &configuration,
                               std::string &error) {
  const auto hash = normalizedSha256(chartSha256);
  if (!hash || !configurationMatchesHash(configuration, *hash, error)) {
    if (!hash) {
      error = "chart SHA-256 must contain 64 hex digits";
    }
    return false;
  }
  PresetData data;
  if (!loadForMutation(practiceDirectory_, *hash, data, error)) {
    return false;
  }
  data.lastUsed = configuration;
  normalizeConfigurationHash(data.lastUsed, *hash);
  return versioned_json::saveAtomic(pathFor(practiceDirectory_, *hash),
                                    presetDataJson(*hash, data), error,
                                    operations_);
}

std::optional<std::string>
PresetStore::saveNamed(std::string_view chartSha256, std::string name,
                       const Configuration &configuration, std::string &error) {
  const auto hash = normalizedSha256(chartSha256);
  const auto normalized = normalizedName(std::move(name));
  if (!hash || !normalized ||
      !configurationMatchesHash(configuration, hash.value_or(""), error)) {
    if (!hash) {
      error = "chart SHA-256 must contain 64 hex digits";
    } else if (!normalized) {
      error = "practice preset name must contain 1 through 128 characters";
    }
    return std::nullopt;
  }
  PresetData data;
  if (!loadForMutation(practiceDirectory_, *hash, data, error,
                       &configuration)) {
    return std::nullopt;
  }
  NamedPreset preset{.id = generatePresetId(data),
                     .name = *normalized,
                     .configuration = configuration};
  normalizeConfigurationHash(preset.configuration, *hash);
  const std::string id = preset.id;
  data.named.push_back(std::move(preset));
  if (!versioned_json::saveAtomic(pathFor(practiceDirectory_, *hash),
                                  presetDataJson(*hash, data), error,
                                  operations_)) {
    return std::nullopt;
  }
  return id;
}

bool PresetStore::updateNamed(std::string_view chartSha256,
                              std::string_view presetId,
                              const Configuration &configuration,
                              std::string &error) {
  const auto hash = normalizedSha256(chartSha256);
  if (!hash ||
      !configurationMatchesHash(configuration, hash.value_or(""), error)) {
    if (!hash) {
      error = "chart SHA-256 must contain 64 hex digits";
    }
    return false;
  }
  PresetData data;
  if (!loadForMutation(practiceDirectory_, *hash, data, error)) {
    return false;
  }
  const auto found = std::ranges::find(data.named, presetId, &NamedPreset::id);
  if (found == data.named.end()) {
    error = "practice preset was not found";
    return false;
  }
  found->configuration = configuration;
  normalizeConfigurationHash(found->configuration, *hash);
  return versioned_json::saveAtomic(pathFor(practiceDirectory_, *hash),
                                    presetDataJson(*hash, data), error,
                                    operations_);
}

bool PresetStore::renameNamed(std::string_view chartSha256,
                              std::string_view presetId, std::string name,
                              std::string &error) {
  const auto hash = normalizedSha256(chartSha256);
  const auto normalized = normalizedName(std::move(name));
  if (!hash || !normalized) {
    error = !hash
                ? "chart SHA-256 must contain 64 hex digits"
                : "practice preset name must contain 1 through 128 characters";
    return false;
  }
  PresetData data;
  if (!loadForMutation(practiceDirectory_, *hash, data, error)) {
    return false;
  }
  const auto found = std::ranges::find(data.named, presetId, &NamedPreset::id);
  if (found == data.named.end()) {
    error = "practice preset was not found";
    return false;
  }
  found->name = *normalized;
  return versioned_json::saveAtomic(pathFor(practiceDirectory_, *hash),
                                    presetDataJson(*hash, data), error,
                                    operations_);
}

bool PresetStore::deleteNamed(std::string_view chartSha256,
                              std::string_view presetId, std::string &error) {
  const auto hash = normalizedSha256(chartSha256);
  if (!hash) {
    error = "chart SHA-256 must contain 64 hex digits";
    return false;
  }
  PresetData data;
  if (!loadForMutation(practiceDirectory_, *hash, data, error)) {
    return false;
  }
  const auto erased = std::erase_if(data.named, [&](const NamedPreset &preset) {
    return preset.id == presetId;
  });
  if (erased == 0) {
    error = "practice preset was not found";
    return false;
  }
  return versioned_json::saveAtomic(pathFor(practiceDirectory_, *hash),
                                    presetDataJson(*hash, data), error,
                                    operations_);
}
} // namespace practice
