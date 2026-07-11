#include "PracticePresetStore.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <limits>
#include <random>
#include <ranges>

namespace practice {
namespace {
using Json = nlohmann::json;
constexpr int kSchemaVersion = 1;

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
  auto loaded = versioned_json::loadAndMigrate(pathFor(directory, *hash),
                                               kSchemaVersion, migrations);
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
    result.data.lastUsed =
        sanitize(std::move(*lastUsed), chartEndMicros).configuration;
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
      preset.configuration =
          sanitize(std::move(*configuration), chartEndMicros).configuration;
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
  return {{"schemaVersion", kSchemaVersion},
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
                     std::string &error) {
  auto loaded =
      loadData(directory, hash, std::numeric_limits<long long>::max());
  if (loaded.status != versioned_json::LoadStatus::Loaded &&
      loaded.status != versioned_json::LoadStatus::Missing) {
    error = loaded.diagnostics.empty() ? "unable to load practice preset file"
                                       : loaded.diagnostics.front();
    return false;
  }
  data = std::move(loaded.data);
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

PresetStore::PresetStore(std::filesystem::path practiceDirectory,
                         const atomic_file::Operations *operations)
    : practiceDirectory_(std::move(practiceDirectory)),
      operations_(operations) {}

PresetLoadResult PresetStore::load(std::string_view chartSha256,
                                   long long chartEndMicros) const {
  return loadData(practiceDirectory_, chartSha256, chartEndMicros);
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
  if (!loadForMutation(practiceDirectory_, *hash, data, error)) {
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
