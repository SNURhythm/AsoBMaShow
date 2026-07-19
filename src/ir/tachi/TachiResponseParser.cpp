#include "TachiResponseParser.h"

#include "../IrOutboxModels.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace ir::tachi {
namespace {

using Json = nlohmann::json;

DeliveryOutcome malformed(std::string_view diagnostic) {
  return {.status = DeliveryStatus::PermanentFailure,
          .code = "malformed_response",
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

DeliveryOutcome rejected(std::string_view diagnostic) {
  return {.status = DeliveryStatus::PermanentFailure,
          .code = "import_rejected",
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

std::string descriptionFrom(const Json &document) {
  const auto found = document.find("description");
  if (found == document.end() || !found->is_string()) {
    return {};
  }
  return sanitizeDiagnostic(found->get_ref<const std::string &>());
}

std::optional<std::string> errorDiagnostic(const Json &errors) {
  if (!errors.is_array()) {
    return std::nullopt;
  }
  std::string diagnostic;
  for (const auto &error : errors) {
    if (!error.is_object()) {
      return std::nullopt;
    }
    const auto message = error.find("message");
    if (message == error.end() || !message->is_string()) {
      return std::nullopt;
    }
    if (!diagnostic.empty()) {
      diagnostic += "\n";
    }
    const auto type = error.find("type");
    if (type != error.end()) {
      if (!type->is_string()) {
        return std::nullopt;
      }
      const auto &typeText = type->get_ref<const std::string &>();
      if (!typeText.empty()) {
        diagnostic += typeText;
        diagnostic += ": ";
      }
    }
    diagnostic += message->get_ref<const std::string &>();
    if (diagnostic.size() > kMaximumDiagnosticBytes * 2) {
      break;
    }
  }
  return sanitizeDiagnostic(diagnostic);
}

DeliveryOutcome parseImportDocument(const Json &document) {
  if (!document.is_object()) {
    return malformed("Tachi Import Document body is not an object");
  }
  const auto scoreIds = document.find("scoreIDs");
  const auto errors = document.find("errors");
  if (scoreIds == document.end() || !scoreIds->is_array() ||
      errors == document.end()) {
    return malformed("Tachi Import Document fields are missing or invalid");
  }
  const auto diagnostic = errorDiagnostic(*errors);
  if (!diagnostic) {
    return malformed("Tachi Import Document errors are invalid");
  }
  std::optional<std::int64_t> parsedUserId;
  const auto userId = document.find("userID");
  if (userId != document.end()) {
    if (userId->is_number_unsigned()) {
      const auto value = userId->get<std::uint64_t>();
      if (value > 0 &&
          value <=
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max())) {
        parsedUserId = static_cast<std::int64_t>(value);
      }
    } else if (userId->is_number_integer()) {
      const auto value = userId->get<std::int64_t>();
      if (value > 0) {
        parsedUserId = value;
      }
    }
  }
  for (const auto &scoreId : *scoreIds) {
    if (!scoreId.is_string()) {
      return malformed("Tachi Import Document score ID is invalid");
    }
    const auto &value = scoreId.get_ref<const std::string &>();
    if (value.empty() || value.size() > kMaximumIrRemoteValueBytes ||
        std::ranges::any_of(value, [](unsigned char character) {
          return character < 0x20U || character == 0x7fU;
        })) {
      return malformed("Tachi Import Document score ID is invalid");
    }
  }
  if (scoreIds->size() == 1) {
    return {.status = DeliveryStatus::Succeeded,
            .remoteUserId = parsedUserId,
            .remoteScoreId = scoreIds->front().get<std::string>(),
            .code =
                diagnostic->empty() ? std::string{} : "accepted_with_warnings",
            .diagnostic = *diagnostic};
  }
  if (scoreIds->empty() && errors->empty()) {
    return {.status = DeliveryStatus::Succeeded,
            .remoteUserId = parsedUserId,
            .code = "already_exists"};
  }
  if (scoreIds->empty()) {
    return rejected(diagnostic->empty() ? "Tachi rejected the score"
                                        : *diagnostic);
  }
  return malformed("Tachi accepted an unexpected number of scores");
}

std::optional<Json> parseObject(std::string_view body,
                                DeliveryOutcome &failure) {
  Json document;
  try {
    document = Json::parse(body);
  } catch (...) {
    failure = malformed("Tachi returned malformed JSON");
    return std::nullopt;
  }
  if (!document.is_object()) {
    failure = malformed("Tachi response is not an object");
    return std::nullopt;
  }
  return document;
}

std::optional<Json> parseEnvelopeDocument(const Json &document,
                                          DeliveryOutcome &failure) {
  const auto success = document.find("success");
  if (success == document.end() || !success->is_boolean()) {
    failure = malformed("Tachi response success field is invalid");
    return std::nullopt;
  }
  if (!success->get<bool>()) {
    const std::string description = descriptionFrom(document);
    failure = rejected(description.empty() ? "Tachi rejected the import"
                                           : description);
    return std::nullopt;
  }
  const auto bodyValue = document.find("body");
  if (bodyValue == document.end() || !bodyValue->is_object()) {
    failure = malformed("Tachi response body is missing or invalid");
    return std::nullopt;
  }
  return *bodyValue;
}

std::optional<Json> parseEnvelope(std::string_view body,
                                  DeliveryOutcome &failure) {
  const auto document = parseObject(body, failure);
  return document ? parseEnvelopeDocument(*document, failure) : std::nullopt;
}

} // namespace

bool isValidImportId(std::string_view value) noexcept {
  return !value.empty() && value.size() <= kMaximumImportIdBytes &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '-' ||
                  character == '_';
         });
}

DeliveryOutcome parseImmediateImportResponse(std::string_view body) noexcept {
  try {
    DeliveryOutcome failure;
    const auto envelopeBody = parseEnvelope(body, failure);
    return envelopeBody ? parseImportDocument(*envelopeBody)
                        : std::move(failure);
  } catch (...) {
    return malformed("Tachi response parsing failed");
  }
}

DeliveryOutcome
parseDeferredImportResponse(std::string_view body,
                            std::string_view requestOrigin) noexcept {
  try {
    DeliveryOutcome failure;
    const auto document = parseObject(body, failure);
    if (!document) {
      return failure;
    }
    std::optional<Json> envelopeBody;
    const Json *queuedImport = &*document;
    if (document->contains("success")) {
      envelopeBody = parseEnvelopeDocument(*document, failure);
      if (!envelopeBody) {
        return failure;
      }
      queuedImport = &*envelopeBody;
    }
    const auto importId = queuedImport->find("importID");
    if (importId == queuedImport->end() || !importId->is_string() ||
        !isValidImportId(importId->get_ref<const std::string &>())) {
      return {.status = DeliveryStatus::PermanentFailure,
              .code = "invalid_import_id",
              .diagnostic = "Tachi queued response has an invalid import ID"};
    }
    return {.status = DeliveryStatus::Deferred,
            .remoteJobId = importId->get<std::string>(),
            .remoteOrigin = std::string(requestOrigin)};
  } catch (...) {
    return malformed("Tachi queued response parsing failed");
  }
}

DeliveryOutcome parsePollStatusResponse(std::string_view body) noexcept {
  try {
    DeliveryOutcome failure;
    const auto envelopeBody = parseEnvelope(body, failure);
    if (!envelopeBody) {
      return failure;
    }
    const auto status = envelopeBody->find("importStatus");
    if (status == envelopeBody->end() || !status->is_string()) {
      return malformed("Tachi poll status is missing or invalid");
    }
    const auto &statusText = status->get_ref<const std::string &>();
    if (statusText == "ongoing") {
      return {.status = DeliveryStatus::Ongoing};
    }
    if (statusText != "completed") {
      return malformed("Tachi poll status is unknown");
    }
    const auto importDocument = envelopeBody->find("import");
    if (importDocument == envelopeBody->end()) {
      return malformed("Tachi completed poll has no Import Document");
    }
    return parseImportDocument(*importDocument);
  } catch (...) {
    return malformed("Tachi poll response parsing failed");
  }
}

std::string parseResponseDescription(std::string_view body) noexcept {
  try {
    const Json document = Json::parse(body);
    return document.is_object() ? descriptionFrom(document) : std::string{};
  } catch (...) {
    return {};
  }
}

} // namespace ir::tachi
