#include "Lr2SkinCsvParser.h"

#include "../../Utils.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>

namespace skin {
namespace {

namespace fs = std::filesystem;

std::string upperAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](char character) {
    return character >= 'a' && character <= 'z'
               ? static_cast<char>(character - ('a' - 'A'))
               : character;
  });
  return value;
}

std::vector<std::string> splitFields(std::string_view line) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = line.find(',', start);
    if (comma == std::string_view::npos) {
      result.emplace_back(line.substr(start));
      return result;
    }
    result.emplace_back(line.substr(start, comma - start));
    start = comma + 1;
  }
}

void replaceAll(std::string &value, std::string_view needle,
                std::string_view replacement) {
  if (needle.empty()) return;
  std::size_t position = 0;
  while ((position = value.find(needle, position)) != std::string::npos) {
    value.replace(position, needle.size(), replacement);
    position += replacement.size();
  }
}

std::string includeReadPath(std::string_view authored) {
  std::string result(authored);
  replaceAll(result, "LR2files\\Theme", "skin");
  std::ranges::replace(result, '\\', '/');
  return result;
}

std::string genericPath(const fs::path &path) {
  return path.lexically_normal().generic_string();
}

std::string includedSourcePath(const LuaSkinFileSystem &fileSystem,
                               std::string_view entryPath,
                               std::string_view readPath) {
  const std::string packagePrefix =
      "skin/" + fileSystem.entry().package.directoryName + "/";
  if (readPath.starts_with(packagePrefix)) {
    return genericPath(fs::path(readPath.substr(packagePrefix.size())));
  }
  if (readPath.starts_with("skin/")) {
    return genericPath(fs::path(readPath));
  }
  const fs::path parent = fs::path(entryPath).parent_path();
  return genericPath(parent / fs::path(readPath));
}

std::string includeChainText(std::span<const std::string> chain) {
  std::ostringstream stream;
  for (std::size_t index = 0; index < chain.size(); ++index) {
    if (index != 0) stream << " -> ";
    stream << chain[index];
  }
  return stream.str();
}

SkinDiagnostic parserDiagnostic(std::string code, std::string message,
                                const SkinSourceLocation &source,
                                DiagnosticSeverity severity =
                                    DiagnosticSeverity::Error) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = source.virtualPath,
          .severity = severity,
          .source = source};
}

std::optional<int> conditionInteger(std::string_view field) {
  std::string numeric;
  numeric.reserve(field.size());
  for (char character : field) {
    if (character == '!') character = '-';
    if ((character >= '0' && character <= '9') || character == '-') {
      numeric.push_back(character);
    }
  }
  if (numeric.empty()) return std::nullopt;
  int result = 0;
  const auto parsed = std::from_chars(
      numeric.data(), numeric.data() + numeric.size(), result);
  return parsed.ec == std::errc{} &&
                 parsed.ptr == numeric.data() + numeric.size()
             ? std::optional<int>(result)
             : std::nullopt;
}

class IncludeControlState final {
public:
  explicit IncludeControlState(std::span<const int> enabled) {
    for (const int option : enabled) options_.insert_or_assign(option, 1);
  }

  void process(const Lr2SkinCommand &command) {
    if (command.name == "IF") {
      ifs_ = evaluate(command);
      skip_ = !ifs_;
    } else if (command.name == "ELSEIF") {
      if (ifs_) {
        skip_ = true;
      } else {
        ifs_ = evaluate(command);
        skip_ = !ifs_;
      }
    } else if (command.name == "ELSE") {
      skip_ = ifs_;
    } else if (command.name == "ENDIF") {
      skip_ = false;
      ifs_ = false;
    } else if (command.name == "SETOPTION" && !skip_ &&
               command.fields.size() >= 2) {
      const auto id = conditionInteger(command.fields[0]);
      const auto enabled = conditionInteger(command.fields[1]);
      if (id && enabled) {
        options_.insert_or_assign(*id, *enabled >= 1 ? 1 : 0);
      }
    }
  }

  [[nodiscard]] bool skipped() const noexcept { return skip_; }

private:
  [[nodiscard]] bool evaluate(const Lr2SkinCommand &command) const {
    for (const auto &field : command.fields) {
      if (field.empty()) continue;
      const auto condition = conditionInteger(field);
      if (!condition || *condition == std::numeric_limits<int>::min()) {
        return false;
      }
      const int id = *condition < 0 ? -*condition : *condition;
      const auto found = options_.find(id);
      if (found == options_.end() ||
          (*condition >= 0 ? found->second != 1 : found->second != 0)) {
        return false;
      }
    }
    return true;
  }

  std::map<int, int> options_;
  bool skip_ = false;
  bool ifs_ = false;
};

class ParseSession final {
public:
  ParseSession(LuaSkinFileSystem &fileSystem,
               const Lr2SkinCsvParserLimits &limits, std::stop_token stop,
               Lr2SkinParseOptions options, Lr2SkinParseResult &result)
      : fileSystem_(fileSystem), limits_(limits), stop_(stop),
        options_(options), control_(options.enabledOptionIds), result_(result) {}

  void parseRoot(std::string entryPath, std::span<const std::byte> bytes) {
    std::ranges::replace(entryPath, '\\', '/');
    entryPath = genericPath(fs::path(entryPath));
    if (cancelled()) return;
    if (limits_.maximumIncludeDepth == 0) {
      result_.fatal = true;
      const SkinSourceLocation source{.virtualPath = entryPath,
                                      .line = 1,
                                      .column = 1};
      result_.diagnostics.push_back(parserDiagnostic(
          "skin_lr2_include_depth", "LR2 root exceeds the include depth limit",
          source));
      return;
    }
    if (bytes.size() > limits_.maximumDocumentBytes) {
      result_.fatal = true;
      const SkinSourceLocation source{.virtualPath = entryPath,
                                      .line = 1,
                                      .column = 1};
      result_.diagnostics.push_back(parserDiagnostic(
          "skin_lr2_byte_limit", "LR2 document exceeds the aggregate byte limit",
          source));
      return;
    }
    consumedBytes_ = bytes.size();
    std::vector<std::string> chain{entryPath};
    parseDocument(entryPath, bytes, chain);
  }

private:
  bool cancelled() {
    if (!stop_.stop_requested()) return false;
    result_.cancelled = true;
    return true;
  }

  void parseDocument(const std::string &sourcePath,
                     std::span<const std::byte> bytes,
                     std::vector<std::string> &chain) {
    if (cancelled()) return;
    const std::string_view encoded(
        reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const auto decoded = cp932_to_utf8(encoded);
    if (!decoded) {
      result_.fatal = true;
      const SkinSourceLocation source{.virtualPath = sourcePath,
                                      .line = 1,
                                      .column = 1};
      result_.diagnostics.push_back(parserDiagnostic(
          "skin_lr2_encoding_invalid", "LR2 document is not valid CP932", source));
      return;
    }
    if (cancelled()) return;

    std::size_t start = 0;
    std::uint32_t lineNumber = 1;
    while (start < decoded->size()) {
      if (cancelled()) return;
      const std::size_t separator = decoded->find_first_of("\r\n", start);
      const std::size_t end = separator == std::string::npos
                                  ? decoded->size()
                                  : separator;
      processLine(std::string_view(*decoded).substr(start, end - start),
                  sourcePath, lineNumber, chain);
      if (result_.cancelled || separator == std::string::npos) return;
      start = separator + 1;
      if ((*decoded)[separator] == '\r' && start < decoded->size() &&
          (*decoded)[start] == '\n') {
        ++start;
      }
      ++lineNumber;
    }
  }

  void processLine(std::string_view line, const std::string &sourcePath,
                   std::uint32_t lineNumber,
                   std::vector<std::string> &chain) {
    if (line.empty() || line.front() != '#') return;
    auto tokens = splitFields(line);
    std::string name = upperAscii(tokens.front().substr(1));
    tokens.erase(tokens.begin());
    const SkinSourceLocation source{.virtualPath = sourcePath,
                                    .line = lineNumber,
                                    .column = 1};
    Lr2SkinCommand command{.name = name,
                           .fields = tokens,
                           .source = source,
                           .includeChain = chain};
    if (options_.includeExpansion == Lr2IncludeExpansionMode::Preserve) {
      result_.commands.push_back(std::move(command));
      return;
    }
    if (options_.includeExpansion ==
        Lr2IncludeExpansionMode::ConditionAware) {
      control_.process(command);
    }
    if (name != "INCLUDE") {
      result_.commands.push_back(std::move(command));
      return;
    }
    if (options_.includeExpansion ==
            Lr2IncludeExpansionMode::ConditionAware &&
        control_.skipped()) {
      return;
    }

    if (tokens.empty() || tokens.front().empty()) {
      result_.diagnostics.push_back(parserDiagnostic(
          "skin_lr2_include_invalid", "LR2 include path is empty", source,
          DiagnosticSeverity::Warning));
      return;
    }
    const std::string readPath = includeReadPath(tokens.front());
    const std::string includedPath =
        includedSourcePath(fileSystem_, chain.front(), readPath);
    std::vector<std::string> nextChain = chain;
    nextChain.push_back(includedPath);
    if (std::ranges::find(chain, includedPath) != chain.end()) {
      result_.fatal = true;
      result_.diagnostics.push_back(parserDiagnostic(
          "skin_lr2_include_cycle",
          "LR2 include cycle: " + includeChainText(nextChain), source));
      return;
    }
    if (chain.size() >= limits_.maximumIncludeDepth) {
      result_.fatal = true;
      result_.diagnostics.push_back(parserDiagnostic(
          "skin_lr2_include_depth",
          "LR2 include depth exceeded: " + includeChainText(nextChain), source));
      return;
    }
    if (cancelled()) return;

    const std::size_t remaining =
        limits_.maximumDocumentBytes - consumedBytes_;
    auto read = fileSystem_.read(readPath, SkinFileUse::LuaModule, remaining);
    if (cancelled()) return;
    if (read.failure) {
      const bool byteLimit = read.failure->code == SkinFileError::LimitExceeded;
      const bool missing = read.failure->code == SkinFileError::Missing;
      result_.fatal = result_.fatal || !missing;
      result_.diagnostics.push_back(parserDiagnostic(
          byteLimit ? "skin_lr2_byte_limit" : "skin_lr2_include_read",
          byteLimit ? "LR2 include exceeds the aggregate byte limit"
                    : "LR2 include could not be read: " + read.failure->message,
          source, missing ? DiagnosticSeverity::Warning
                          : DiagnosticSeverity::Error));
      return;
    }
    consumedBytes_ += read.bytes.size();
    parseDocument(includedPath, read.bytes, nextChain);
  }

  LuaSkinFileSystem &fileSystem_;
  const Lr2SkinCsvParserLimits &limits_;
  std::stop_token stop_;
  Lr2SkinParseOptions options_;
  IncludeControlState control_;
  Lr2SkinParseResult &result_;
  std::size_t consumedBytes_ = 0;
};

} // namespace

Lr2SkinParseResult Lr2SkinCsvParser::parse(
    LuaSkinFileSystem &fileSystem, std::string_view entryPath,
    std::span<const std::byte> cp932Bytes, std::stop_token stop,
    Lr2SkinParseOptions options) const {
  Lr2SkinParseResult result;
  ParseSession session(fileSystem, limits_, stop, options, result);
  session.parseRoot(std::string(entryPath), cp932Bytes);
  return result;
}

} // namespace skin
