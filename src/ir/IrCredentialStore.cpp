#include "IrCredentialStore.h"

#include "../VersionedJson.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace ir {
namespace {

using nlohmann::json;

bool validProviderId(std::string_view value) {
  if (value.empty() || value.size() > 64 ||
      !(value.front() >= 'a' && value.front() <= 'z')) {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-';
  });
}

bool validApiKey(std::string_view value) {
  return !value.empty() &&
         value.size() <= IrCredentialStore::kMaximumApiKeyBytes &&
         std::ranges::none_of(value, [](unsigned char character) {
           return character == 0;
         });
}

std::string redactKeys(std::string diagnostic,
                       const IrCredentials &credentials) {
  for (const auto &[providerId, key] : credentials.apiKeys) {
    (void)providerId;
    if (key.empty()) {
      continue;
    }
    std::size_t offset = 0;
    while ((offset = diagnostic.find(key, offset)) != std::string::npos) {
      diagnostic.replace(offset, key.size(), "[redacted]");
      offset += 10;
    }
  }
  return diagnostic;
}

IrCredentialWriteResult invalidWrite(std::string diagnostic) {
  return {.succeeded = false, .diagnostic = std::move(diagnostic)};
}

bool validateCredentials(const IrCredentials &credentials,
                         std::string &diagnostic) {
  if (credentials.apiKeys.size() > 64) {
    diagnostic = "credential provider count exceeds the supported limit";
    return false;
  }
  for (const auto &[providerId, apiKey] : credentials.apiKeys) {
    if (!validProviderId(providerId)) {
      diagnostic = "credential provider ID is invalid";
      return false;
    }
    if (!validApiKey(apiKey)) {
      diagnostic = "credential API key length is invalid";
      return false;
    }
  }
  return true;
}

json credentialsToJson(const IrCredentials &credentials) {
  json providers = json::object();
  for (const auto &[providerId, apiKey] : credentials.apiKeys) {
    providers[providerId] = {{"apiKey", apiKey}};
  }
  return {{"schemaVersion", IrCredentialStore::kCurrentSchemaVersion},
          {"providers", std::move(providers)}};
}

} // namespace

IrCredentialLoadResult
IrCredentialStore::load(const std::filesystem::path &path) {
  IrCredentialLoadResult result;
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) {
    result.status = IrCredentialLoadStatus::Invalid;
    result.diagnostics.push_back("credential file could not be inspected");
    return result;
  }
  if (!exists) {
    result.status = IrCredentialLoadStatus::Missing;
    return result;
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > kMaximumFileBytes) {
    result.status = IrCredentialLoadStatus::Invalid;
    result.diagnostics.push_back(error ? "credential file size is unavailable"
                                       : "credential file exceeds size limit");
    return result;
  }

  const std::array<versioned_json::Migration, 1> migrations = {
      [](json &document, std::string &) {
        if (!document.contains("providers")) {
          document["providers"] = json::object();
        }
        return true;
      }};
  auto loaded =
      versioned_json::loadAndMigrate(path, kCurrentSchemaVersion, migrations);
  if (loaded.status == versioned_json::LoadStatus::Missing) {
    result.status = IrCredentialLoadStatus::Missing;
    return result;
  }
  if (loaded.status == versioned_json::LoadStatus::FutureVersion) {
    result.status = IrCredentialLoadStatus::FutureVersion;
    result.diagnostics = std::move(loaded.diagnostics);
    return result;
  }
  if (loaded.status != versioned_json::LoadStatus::Loaded) {
    result.status = IrCredentialLoadStatus::Invalid;
    result.diagnostics = std::move(loaded.diagnostics);
    return result;
  }

  const auto providers = loaded.document.find("providers");
  if (providers == loaded.document.end() || !providers->is_object()) {
    result.status = IrCredentialLoadStatus::Invalid;
    result.diagnostics.push_back("credential providers must be an object");
    return result;
  }
  IrCredentials credentials;
  for (const auto &[providerId, provider] : providers->items()) {
    if (!validProviderId(providerId) || !provider.is_object()) {
      result.status = IrCredentialLoadStatus::Invalid;
      result.diagnostics.push_back("credential provider entry is invalid");
      return result;
    }
    const auto apiKey = provider.find("apiKey");
    if (apiKey == provider.end() || !apiKey->is_string()) {
      result.status = IrCredentialLoadStatus::Invalid;
      result.diagnostics.push_back("credential API key must be a string");
      return result;
    }
    const std::string value = apiKey->get<std::string>();
    if (!validApiKey(value)) {
      result.status = IrCredentialLoadStatus::Invalid;
      result.diagnostics.push_back("credential API key length is invalid");
      return result;
    }
    credentials.apiKeys.emplace(providerId, value);
  }
  result.credentials = std::move(credentials);
  result.status = IrCredentialLoadStatus::Loaded;
  return result;
}

IrCredentialWriteResult IrCredentialStore::save(
    const std::filesystem::path &path, const IrCredentials &credentials,
    const atomic_file::Operations *operations) {
  std::string diagnostic;
  if (!validateCredentials(credentials, diagnostic)) {
    return invalidWrite(redactKeys(std::move(diagnostic), credentials));
  }
  const json document = credentialsToJson(credentials);
  if (document.dump(2).size() + 1 > kMaximumFileBytes) {
    return invalidWrite("credential file exceeds size limit");
  }
  if (!versioned_json::saveAtomic(path, document, diagnostic, operations)) {
    return invalidWrite(redactKeys(std::move(diagnostic), credentials));
  }
  return {.succeeded = true};
}

IrCredentialWriteResult IrCredentialStore::replaceApiKey(
    const std::filesystem::path &path, std::string providerId,
    std::string apiKey, const atomic_file::Operations *operations) {
  if (!validProviderId(providerId)) {
    return invalidWrite("credential provider ID is invalid");
  }
  if (!validApiKey(apiKey)) {
    return invalidWrite("credential API key length is invalid");
  }
  auto loaded = load(path);
  if (loaded.status != IrCredentialLoadStatus::Loaded &&
      loaded.status != IrCredentialLoadStatus::Missing) {
    return invalidWrite("credential file cannot be safely updated");
  }
  loaded.credentials.apiKeys[std::move(providerId)] = std::move(apiKey);
  return save(path, loaded.credentials, operations);
}

IrCredentialWriteResult IrCredentialStore::removeApiKey(
    const std::filesystem::path &path, std::string providerId,
    const atomic_file::Operations *operations) {
  if (!validProviderId(providerId)) {
    return invalidWrite("credential provider ID is invalid");
  }
  auto loaded = load(path);
  if (loaded.status == IrCredentialLoadStatus::Missing) {
    return {.succeeded = true};
  }
  if (loaded.status != IrCredentialLoadStatus::Loaded) {
    return invalidWrite("credential file cannot be safely updated");
  }
  loaded.credentials.apiKeys.erase(providerId);
  return save(path, loaded.credentials, operations);
}

} // namespace ir
