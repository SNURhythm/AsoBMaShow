#include "BmsSyntax.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace bms_syntax {
namespace {
bool space(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool idCharacter(char c) {
  return digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
bool equalCommand(std::string_view value, std::string_view upper) {
  if (value.size() != upper.size()) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i] >= 'a' && value[i] <= 'z' ? value[i] - ('a' - 'A') : value[i];
    if (c != upper[i]) return false;
  }
  return true;
}
template <std::size_t N>
bool oneOf(std::string_view value, const std::string_view (&commands)[N]) {
  for (auto command : commands) if (equalCommand(value, command)) return true;
  return false;
}
bool number(std::string_view value) {
  std::size_t i = 0, digits = 0;
  if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
  while (i < value.size() && digit(value[i])) { ++i; ++digits; }
  if (i < value.size() && value[i] == '.') {
    ++i;
    while (i < value.size() && digit(value[i])) { ++i; ++digits; }
  }
  if (digits == 0) return false;
  if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
    ++i;
    if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
    const auto start = i;
    while (i < value.size() && digit(value[i])) ++i;
    if (i == start) return false;
  }
  return i == value.size();
}
}

std::shared_ptr<const Document> tokenize(std::string_view text) {
  auto document = std::make_shared<Document>();
  if (text.size() > std::numeric_limits<std::uint32_t>::max()) return document;
  document->spans.reserve(std::min<std::size_t>(4096, text.size() / 16));
  std::size_t cursor = 0;
  std::uint64_t units = 0;
  const auto advance = [&](std::size_t target) {
    // Text is already decoded UTF-8. Count code units once, including gaps
    // between tokens. ASCII words avoid a branch per data cell/character.
    while (cursor < target) {
      if (target - cursor >= sizeof(std::uint64_t)) {
        std::uint64_t word;
        std::memcpy(&word, text.data() + cursor, sizeof(word));
        if ((word & UINT64_C(0x8080808080808080)) == 0) {
          cursor += sizeof(word);
          units += sizeof(word);
          continue;
        }
      }
      const auto c = static_cast<unsigned char>(text[cursor++]);
      if ((c & 0xc0) != 0x80) units += c >= 0xf0 && c <= 0xf4 ? 2 : 1;
    }
  };
  const auto emit = [&](std::size_t begin, std::size_t end, Kind kind) {
    if (begin == end) return;
    advance(begin);
    const auto first = units;
    advance(end);
    if (units <= std::numeric_limits<std::uint32_t>::max())
      document->spans.push_back({static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(units), kind});
  };
  constexpr std::string_view controls[] = {"#RANDOM", "#SETRANDOM", "#ENDRANDOM", "#IF", "#ELSEIF",
      "#ELSE", "#ENDIF", "#SWITCH", "#SETSWITCH", "#CASE", "#SKIP", "#DEF", "#ENDSW"};
  constexpr std::string_view resources[] = {"#WAV", "#BMP", "#BPM", "#EXBPM", "#STOP", "#SCROLL",
      "#SPEED", "#BGA", "#TEXT"};
  constexpr std::string_view textHeaders[] = {"#TITLE", "#SUBTITLE", "#ARTIST", "#SUBARTIST", "#GENRE",
      "#COMMENT", "#STAGEFILE", "#BACKBMP", "#BANNER", "#PREVIEW", "#MIDI", "#VIDEOFILE", "#PATH_WAV",
      "#MAKER", "#CHARTNAME"};
  for (std::size_t line = 0; line < text.size();) {
    auto end = line;
    while (end < text.size() && text[end] != '\r' && text[end] != '\n') ++end;
    auto begin = line, contentEnd = end;
    if (begin == 0 && text.substr(0, 3) == "\xef\xbb\xbf") begin += 3;
    while (begin < contentEnd && space(text[begin])) ++begin;
    while (contentEnd > begin && space(text[contentEnd - 1])) --contentEnd;
    if (begin < contentEnd) {
      if (text[begin] != '#') {
        emit(begin, contentEnd, Kind::Comment);
      } else if (contentEnd - begin >= 7 && digit(text[begin + 1]) && digit(text[begin + 2]) &&
          digit(text[begin + 3]) && idCharacter(text[begin + 4]) && idCharacter(text[begin + 5]) &&
          text[begin + 6] == ':') {
        emit(begin, begin + 4, Kind::Number);
        emit(begin + 4, begin + 6, Kind::Identifier);
        const bool measureLength = text[begin + 4] == '0' && text[begin + 5] == '2';
        emit(begin + 7, contentEnd, measureLength ? Kind::Number : Kind::Data);
      } else {
        auto commandEnd = begin + 1;
        while (commandEnd < contentEnd && !space(text[commandEnd])) ++commandEnd;
        const auto command = text.substr(begin, commandEnd - begin);
        auto value = commandEnd;
        while (value < contentEnd && space(text[value])) ++value;
        bool indexed = false;
        Kind valueKind = Kind::String;
        for (auto prefix : resources) {
          if (command.size() == prefix.size() + 2 && equalCommand(command.substr(0, prefix.size()), prefix) &&
              idCharacter(command[command.size() - 2]) && idCharacter(command.back())) {
            emit(begin, commandEnd - 2, Kind::Directive);
            emit(commandEnd - 2, commandEnd, Kind::Identifier);
            indexed = true;
            if (prefix != "#WAV" && prefix != "#BMP" && prefix != "#TEXT") valueKind = Kind::Number;
            break;
          }
        }
        if (!indexed) {
          emit(begin, commandEnd, oneOf(command, controls) ? Kind::Control : Kind::Directive);
          if (equalCommand(command, "#LNOBJ")) valueKind = Kind::Identifier;
          else if (!oneOf(command, textHeaders) && number(text.substr(value, contentEnd - value)))
            valueKind = Kind::Number;
        }
        emit(value, contentEnd, valueKind);
      }
    }
    line = end;
    if (line < text.size() && text[line] == '\r') ++line;
    if (line < text.size() && text[line] == '\n') ++line;
  }
  advance(text.size());
  if (units > std::numeric_limits<std::uint32_t>::max()) document->spans.clear();
  else document->length = static_cast<std::uint32_t>(units);
  return document;
}

PaintWindow paintWindow(const Document &document, std::size_t visibleBegin, std::size_t visibleEnd) {
  const std::size_t length = document.length;
  visibleBegin = std::min(visibleBegin, length);
  visibleEnd = std::clamp(visibleEnd, visibleBegin, length);
  const auto begin = visibleBegin - std::min<std::size_t>(paintOverscan, visibleBegin);
  auto end = std::min(visibleEnd + std::min<std::size_t>(paintOverscan, length - visibleEnd),
      begin + std::min<std::size_t>(maxPaintCharacters, length - begin));
  const auto first = std::lower_bound(document.spans.begin(), document.spans.end(), begin,
      [](const Span &span, std::size_t position) { return span.end <= position; });
  auto last = std::lower_bound(first, document.spans.end(), end,
      [](const Span &span, std::size_t position) { return span.begin < position; });
  if (static_cast<std::size_t>(last - first) > maxPaintSpans) {
    last = first + maxPaintSpans;
    end = std::min<std::size_t>(end, (last - 1)->end);
  }
  return {static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end),
          static_cast<std::size_t>(first - document.spans.begin()),
          static_cast<std::size_t>(last - document.spans.begin())};
}
}
