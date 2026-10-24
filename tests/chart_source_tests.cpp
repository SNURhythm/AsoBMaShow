#include "scene/ChartSourceDocument.h"
#include <atomic>
#include <cassert>
#include <memory>

int main() {
  const std::string source =
      "#CHARSET UTF-8\r\n#TITLE 한글\r\n#BPM 120\r\n#STOP01 48\r\n"
      "#00011:01020304\r\n#00009:00010000\r\n"
      "#00102:0.5\r\n#00111:0506\r\n"
      "#RANDOM 2\r\n#IF 1\r\n#00112:0708\r\n#ENDIF\r\n"
      "#IF 2\r\n#00112:090A\r\n#ENDIF\r\n#ENDRANDOM\r\n";
  const std::vector<unsigned char> bytes(source.begin(), source.end());
  bms_parser::Parser parser;
  parser.SetRandomValues({2});
  bms_parser::Chart *raw = nullptr;
  std::atomic_bool cancelled = false;
  parser.Parse(bytes, &raw, false, false, cancelled);
  std::unique_ptr<bms_parser::Chart> chart(raw);
  assert(chart && chart->Measures.size() >= 2);
  const auto document = chart_source::decode(bytes);
  assert(document.find('\r') == std::string::npos);
  assert(document.find("한글") != std::string::npos);
  const auto selected = [&](long long start, long long end) {
    std::vector<std::string> values;
    for (const auto &range : chart_source::highlights(document, *chart, start, end))
      values.push_back(document.substr(range.begin, range.end - range.begin));
    return values;
  };
  // STOP at 0.5 s delays the following cell to 1.5 s; end is exclusive.
  assert((selected(1500000, 2000000) == std::vector<std::string>{"03", "00"}));
  assert((selected(600000, 900000).empty()));
  // A half-length measure and the selected random branch share the loaded timing.
  assert((selected(2500000, 3000000) == std::vector<std::string>{"0.5", "05", "09"}));
  assert((selected(3000000, 3500000) == std::vector<std::string>{"06", "0A"}));
  assert(selected(3000000, 3000000).empty());
  assert(selected(9000000, 10000000).empty());
  assert(chart_source::decode({}).empty());

  const std::string bpmSource = "#BPM 120\n#00003:00F0\n#00011:01020304\n#00006:0102\n";
  const std::vector<unsigned char> bpmBytes(bpmSource.begin(), bpmSource.end());
  raw = nullptr;
  parser.Parse(bpmBytes, &raw, false, false, cancelled);
  std::unique_ptr<bms_parser::Chart> bpmChart(raw);
  auto ranges = chart_source::highlights(bpmSource, *bpmChart, 1250000, 1500000);
  assert(ranges.size() == 1 && bpmSource.substr(ranges[0].begin, ranges[0].end - ranges[0].begin) == "04");
  ranges = chart_source::highlights(bpmSource, *bpmChart, 0, 1);
  assert(ranges.size() == 3 && bpmSource.substr(ranges.back().begin, ranges.back().end - ranges.back().begin) == "0102");

  // The loader evaluates the innermost IF even within a skipped outer IF,
  // consumes nested RANDOMs, and ignores ELSE directives.
  const std::string nested = "#BPM 120\n#RANDOM 2\n#IF 1\n#RANDOM 2\n#IF 2\n"
      "#00011:01\n#ELSE\n#00012:02\n#ENDIF\n#ENDRANDOM\n#00013:03\n#ENDIF\n";
  parser.SetRandomValues({2, 2});
  raw = nullptr;
  parser.Parse(std::vector<unsigned char>(nested.begin(), nested.end()), &raw, false, false, cancelled);
  std::unique_ptr<bms_parser::Chart> nestedChart(raw);
  ranges = chart_source::highlights(nested, *nestedChart, 0, 1000000);
  assert(ranges.size() == 2);
  assert(nested.substr(ranges[0].begin, ranges[0].end - ranges[0].begin) == "01");
  assert(nested.substr(ranges[1].begin, ranges[1].end - ranges[1].begin) == "02");

  // Random directive arguments use the same ASCII/control trimming as the loader.
  parser.SetRandomValues({2});
  for (const auto &[randomArgument, ifArgument] :
       std::vector<std::pair<std::string, std::string>>{
           {" 2", "2"}, {"2\t ", "2"}, {"2", " \t+2\t "},
           {"\t+2\x1f", "\x1f" "2\t"}}) {
    const std::string spaced = "#BPM 120\n#RANDOM " + randomArgument +
        "\n#IF 1\n#00011:01\n#ENDIF\n#IF " + ifArgument +
        "\n#00011:02\n#ENDIF\n#ENDRANDOM\n";
    raw = nullptr;
    parser.Parse(std::vector<unsigned char>(spaced.begin(), spaced.end()), &raw, false, false, cancelled);
    std::unique_ptr<bms_parser::Chart> spacedChart(raw);
    assert(spacedChart && spacedChart->Meta.RandomValues == std::vector<int>{2});
    ranges = chart_source::highlights(spaced, *spacedChart, 0, 1000000);
    assert(ranges.size() == 1);
    assert(spaced.substr(ranges[0].begin, ranges[0].end - ranges[0].begin) == "02");
  }
  const std::string emptyArguments = "#BPM 120\n#RANDOM \t \n#IF \t \n#00011:01\n#ENDIF\n";
  raw = nullptr;
  parser.Parse(std::vector<unsigned char>(emptyArguments.begin(), emptyArguments.end()), &raw, false, false, cancelled);
  std::unique_ptr<bms_parser::Chart> emptyArgumentsChart(raw);
  assert(emptyArgumentsChart && emptyArgumentsChart->Meta.RandomValues.empty());
  ranges = chart_source::highlights(emptyArguments, *emptyArgumentsChart, 0, 1000000);
  assert(ranges.size() == 1);

  // UTF-16 follows the chart loader's BOM detection and CR-only lines work too.
  const std::u16string utf16 = u"#TITLE 日本語\r#BPM 120\r#00011:01\r";
  std::vector<unsigned char> utf16Bytes{0xff, 0xfe};
  for (const auto unit : utf16) {
    utf16Bytes.push_back(unit & 0xff);
    utf16Bytes.push_back(unit >> 8);
  }
  const auto utf16Text = chart_source::decode(utf16Bytes);
  assert(utf16Text.find("日本語") != std::string::npos);
  assert(utf16Text.find('\r') == std::string::npos);
  raw = nullptr;
  parser.Parse(utf16Bytes, &raw, false, false, cancelled);
  std::unique_ptr<bms_parser::Chart> utf16Chart(raw);
  ranges = chart_source::highlights(utf16Text, *utf16Chart, 0, 1000000);
  assert(ranges.size() == 1 && utf16Text.substr(ranges[0].begin, ranges[0].end - ranges[0].begin) == "01");
}
