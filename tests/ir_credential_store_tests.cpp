#include "ir/IrCredentialStore.h"

#include "AtomicFile.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <tuple>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  TempDirectory() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-ir-credentials-" + std::to_string(nonce));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void writeFile(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

void expectNoBackupArtifacts(const std::filesystem::path &path,
                             std::string_view operation) {
  for (const std::string_view suffix :
       {".bak", ".bak.pending", ".bak.previous"}) {
    expect(!std::filesystem::exists(path.string() + std::string(suffix)),
           std::string(operation) + " removes credential artifact " +
               std::string(suffix));
  }
}

void testMissingSaveLoadReplaceAndRemove() {
  TempDirectory temp;
  const auto path = temp.path() / "ir-credentials.json";
  const auto missing = ir::IrCredentialStore::load(path);
  expect(missing.status == ir::IrCredentialLoadStatus::Missing,
         "missing credential file is normal");
  expect(missing.credentials.apiKeys.empty(),
         "missing credential file returns no keys");

  const auto first = ir::IrCredentialStore::replaceApiKey(
      path, "tachi", "first-device-local-key");
  expect(first.succeeded, "first key replacement creates the file");
  auto loaded = ir::IrCredentialStore::load(path);
  expect(loaded.status == ir::IrCredentialLoadStatus::Loaded,
         "saved credentials load");
  expect(loaded.credentials.apiKeys.at("tachi") ==
             "first-device-local-key",
         "saved API key round trips");

  for (const std::string_view suffix :
       {".bak", ".bak.pending", ".bak.previous"}) {
    writeFile(path.string() + std::string(suffix), "legacy-secret-artifact");
  }
  expect(ir::IrCredentialStore::replaceApiKey(
             path, "tachi", "replacement-device-local-key")
             .succeeded,
         "existing key is replaceable");
  loaded = ir::IrCredentialStore::load(path);
  expect(loaded.credentials.apiKeys.at("tachi") ==
             "replacement-device-local-key",
         "replacement key becomes current");
  expectNoBackupArtifacts(path, "key replacement");

  for (const std::string_view suffix :
       {".bak", ".bak.pending", ".bak.previous"}) {
    writeFile(path.string() + std::string(suffix), "legacy-secret-artifact");
  }
  expect(ir::IrCredentialStore::removeApiKey(path, "tachi").succeeded,
         "key removal succeeds");
  loaded = ir::IrCredentialStore::load(path);
  expect(loaded.status == ir::IrCredentialLoadStatus::Loaded &&
             loaded.credentials.apiKeys.empty(),
         "key removal persists an empty provider map");
  expectNoBackupArtifacts(path, "key removal");
}

void testMalformedFutureAndOversizedFilesFailClosed() {
  TempDirectory temp;
  for (const auto &[name, contents, expected] : {
           std::tuple<std::string_view, std::string_view,
                      ir::IrCredentialLoadStatus>{
               "malformed.json", "{not-json",
               ir::IrCredentialLoadStatus::Invalid},
           {"future.json", R"({"schemaVersion":2,"providers":{}})",
            ir::IrCredentialLoadStatus::FutureVersion},
           {"wrong-shape.json",
            R"({"schemaVersion":1,"providers":{"tachi":{"apiKey":7}}})",
            ir::IrCredentialLoadStatus::Invalid},
       }) {
    const auto path = temp.path() / name;
    writeFile(path, contents);
    const auto loaded = ir::IrCredentialStore::load(path);
    expect(loaded.status == expected, "invalid credential file fails closed");
    expect(loaded.credentials.apiKeys.empty(),
           "invalid credential file exposes no partial keys");
  }

  const auto oversizedPath = temp.path() / "oversized.json";
  writeFile(oversizedPath,
            std::string(ir::IrCredentialStore::kMaximumFileBytes + 1, 'x'));
  expect(ir::IrCredentialStore::load(oversizedPath).status ==
             ir::IrCredentialLoadStatus::Invalid,
         "oversized credential file fails before parsing");
}

void testKeyAndProviderValidationDoesNotLeakSecrets() {
  TempDirectory temp;
  const auto path = temp.path() / "ir-credentials.json";
  const std::string secret = "sentinel-api-key-that-must-not-be-diagnosed";
  for (const auto result : {
           ir::IrCredentialStore::replaceApiKey(path, "tachi", ""),
           ir::IrCredentialStore::replaceApiKey(
               path, "tachi",
               std::string(ir::IrCredentialStore::kMaximumApiKeyBytes + 1,
                           's')),
           ir::IrCredentialStore::replaceApiKey(path, "bad/provider", secret),
       }) {
    expect(!result.succeeded, "invalid key update is rejected");
    expect(result.diagnostic.find(secret) == std::string::npos,
           "write diagnostic does not echo API key material");
  }
  expect(!std::filesystem::exists(path),
         "invalid key updates do not create a credential file");

  const std::string maximumKey(ir::IrCredentialStore::kMaximumApiKeyBytes,
                               'k');
  expect(ir::IrCredentialStore::replaceApiKey(path, "tachi", maximumKey)
             .succeeded,
         "a key at the four KiB limit is accepted");
}

void testWhitespaceAndControlBytesAreRejectedOnSaveAndLoad() {
  TempDirectory temp;
  const std::array invalidKeys = {
      std::pair{std::string_view("leading-space"), std::string(" leading")},
      std::pair{std::string_view("trailing-space"), std::string("trailing ")},
      std::pair{std::string_view("newline"), std::string("line\nbreak")},
      std::pair{std::string_view("control"),
                std::string("key") + static_cast<char>(0x01) + "value"},
      std::pair{std::string_view("del"),
                std::string("key") + static_cast<char>(0x7f) + "value"},
  };
  for (const auto &[name, apiKey] : invalidKeys) {
    const auto path = temp.path() / ("save-" + std::string(name) + ".json");
    const auto result =
        ir::IrCredentialStore::replaceApiKey(path, "tachi", apiKey);
    expect(!result.succeeded,
           "credential save rejects " + std::string(name));
    expect(!std::filesystem::exists(path),
           "rejected credential save creates no file");
  }

  const std::array invalidJsonKeys = {
      std::pair{std::string_view("leading-space"), std::string(" leading")},
      std::pair{std::string_view("trailing-space"), std::string("trailing ")},
      std::pair{std::string_view("newline"), std::string("line\\nbreak")},
      std::pair{std::string_view("control"), std::string("key\\u0001value")},
      std::pair{std::string_view("del"),
                std::string("key") + static_cast<char>(0x7f) + "value"},
  };
  for (const auto &[name, encodedApiKey] : invalidJsonKeys) {
    const auto path = temp.path() / ("load-" + std::string(name) + ".json");
    writeFile(path,
              "{\"schemaVersion\":1,\"providers\":{\"tachi\":{\"apiKey\":\"" +
                  encodedApiKey + "\"}}}");
    const auto loaded = ir::IrCredentialStore::load(path);
    expect(loaded.status == ir::IrCredentialLoadStatus::Invalid,
           "credential load rejects " + std::string(name));
    expect(loaded.credentials.apiKeys.empty(),
           "invalid loaded credential exposes no keys");
  }
}

void testAtomicFailureRollsBackPriorCredentials() {
  TempDirectory temp;
  const auto path = temp.path() / "ir-credentials.json";
  expect(ir::IrCredentialStore::replaceApiKey(path, "tachi", "old-key")
             .succeeded,
         "rollback fixture saves initial credentials");

  atomic_file::Operations operations = atomic_file::defaultOperations();
  const auto realReplace = operations.replace;
  operations.replace = [&](const auto &from, const auto &to,
                           std::string &error) {
    if (from == path.string() + ".tmp" && to == path) {
      error = "injected credential replacement failure";
      return false;
    }
    return realReplace(from, to, error);
  };

  ir::IrCredentials replacement;
  replacement.apiKeys["tachi"] = "new-secret-key";
  const auto failed =
      ir::IrCredentialStore::save(path, replacement, &operations);
  expect(!failed.succeeded, "injected credential write fails");
  expect(failed.diagnostic.find("new-secret-key") == std::string::npos,
         "atomic failure diagnostic excludes the replacement key");
  const auto loaded = ir::IrCredentialStore::load(path);
  expect(loaded.status == ir::IrCredentialLoadStatus::Loaded &&
             loaded.credentials.apiKeys.at("tachi") == "old-key",
         "atomic failure restores the prior credential file");
}

} // namespace

int main() {
  testMissingSaveLoadReplaceAndRemove();
  testMalformedFutureAndOversizedFilesFailClosed();
  testKeyAndProviderValidationDoesNotLeakSecrets();
  testWhitespaceAndControlBytesAreRejectedOnSaveAndLoad();
  testAtomicFailureRollsBackPriorCredentials();
  if (failures != 0) {
    std::cerr << failures << " IR credential store test(s) failed\n";
    return 1;
  }
  std::cout << "IR credential store tests passed\n";
  return 0;
}
