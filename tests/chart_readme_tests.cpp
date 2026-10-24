#include "library/ChartReadme.h"
#include "ArchiveFile.h"
#include "ArchiveRAII.h"
#include "StorageTestSupport.h"
#include "text/Utf8.h"
#include <archive_entry.h>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <thread>

using storage_test::require;
namespace {
void writeZip(const std::filesystem::path &path) {
  auto writer = makeArchiveWriteHandle();
  require(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK, "zip format");
  require(archive_write_open_filename(writer.get(), path.string().c_str()) == ARCHIVE_OK, "zip open");
  for (const auto *name : {"song/chart.bms", "song/Artist.TXT", "song/sub/notes.txt", "other/readme.txt", "song/music.wav", "large/too-big.txt"}) {
    auto entry = std::unique_ptr<archive_entry, decltype(&archive_entry_free)>(archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), name);
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    const bool oversized = std::string_view(name) == "large/too-big.txt";
    const std::string contents(oversized ? chart_readme::maximumBytes + 1 : 5, 'x');
    archive_entry_set_size(entry.get(), contents.size());
    require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK, "zip header");
    const auto payload = oversized ? std::string_view(contents) : std::string_view("notes");
    require(archive_write_data(writer.get(), payload.data(), payload.size()) == static_cast<la_ssize_t>(payload.size()), "zip data");
  }
  require(archive_write_close(writer.get()) == ARCHIVE_OK, "zip close");
}
void testDiscoveryAndReading() {
  storage_test::TempDirectory dir;
  dir.write("song/chart.bms"); dir.write("song/Artist.TXT", "hello\r\nartist\t!\rbye");
  dir.write("song/readme.txt", "한글"); dir.write("song/nested/ignore.txt");
  dir.write("song/no.md"); dir.write("outside.txt");
  std::filesystem::create_directory(dir.root / "song/fake.txt");
  const auto listed = chart_readme::list(dir.root / "song/chart.bms");
  require(listed.error == chart_readme::Error::None && listed.paths.size() == 2,
          "all immediate regular .txt siblings, ignoring case, are discovered");
  require(listed.paths[0].filename() == "Artist.TXT" && listed.paths[1].filename() == "readme.txt",
          "documents have stable filename order");
  require(chart_readme::read(listed.paths[0]).text == "hello\nartist    !\nbye", "line endings and tabs normalize");
  require(chart_readme::read(listed.paths[1]).text == "한글", "UTF-8 Korean preserved");
  require(chart_readme::list(dir.root / "absent/chart.bms").error == chart_readme::Error::Unavailable,
          "unavailable folder reported");
  std::stop_source cancelled; cancelled.request_stop();
  require(chart_readme::list(dir.root / "song/chart.bms", cancelled.get_token()).error == chart_readme::Error::Cancelled,
          "cancelled discovery stops");
  require(chart_readme::read(listed.paths[0], cancelled.get_token()).error == chart_readme::Error::Cancelled,
          "cancelled read stops");
  dir.write("large.txt", "x");
  std::filesystem::resize_file(dir.root / "large.txt", chart_readme::maximumBytes + 1);
  require(chart_readme::read(dir.root / "large.txt").error != chart_readme::Error::None, "oversized file rejected before display");
  dir.write("empty.txt", "");
  require(chart_readme::read(dir.root / "empty.txt").error == chart_readme::Error::None, "empty file is readable");

  writeZip(dir.root / "charts.zip");
  std::vector<archive_file::Entry> entries;
  require(archive_file::listEntries(dir.root / "charts.zip", entries), "prime archive index");
  const auto opens = archive_file::zipStreamingOpenCountForTesting();
  const auto archived = chart_readme::list(archive_file::makeVirtualPath(dir.root / "charts.zip", "song/chart.bms"));
  require(archived.error == chart_readme::Error::None && archived.paths.size() == 1, "archive discovery stays in chart inner folder");
  require(archive_file::zipStreamingOpenCountForTesting() == opens, "archive discovery reads metadata without payload extraction");
  require(chart_readme::read(archived.paths[0]).text == "notes", "selected archived document is readable");
  require(chart_readme::read(archive_file::makeVirtualPath(dir.root / "charts.zip", "large/too-big.txt")).error == chart_readme::Error::TooLarge,
          "archive size limit has a specific user-facing error");
}
void testLatestLoaderRequestWins() {
  storage_test::TempDirectory dir;
  dir.write("one/chart.bms"); dir.write("one/readme.txt", "first");
  dir.write("two/chart.bms"); dir.write("two/readme.txt", "second");
  chart_readme::Loader loader;
  loader.open(dir.root / "one/chart.bms");
  loader.cancel();
  loader.open(dir.root / "two/chart.bms");
  const auto waitForResult = [&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      if (auto result = loader.takeResult()) return *result;
      std::this_thread::yield();
    }
    throw std::runtime_error("reader did not complete");
  };
  const auto reopened = waitForResult();
  require(reopened.discovered && reopened.content.text == "second" &&
          reopened.documents.paths.front() == dir.root / "two/readme.txt",
          "reopening discards prior chart results");
  loader.select(dir.root / "one/readme.txt");
  const auto selected = waitForResult();
  require(!selected.discovered && selected.content.text == "first", "file selection reads the chosen document");
  dir.write("korean.txt", "\xc7\xd1\xb1\xdb");
  loader.select(dir.root / "korean.txt", chart_readme::Encoding::Korean);
  require(waitForResult().content.text == "한글", "encoding selection reaches the background reader");
  loader.cancel();
  require(!loader.takeResult(), "cancel clears ready results");
}
void testEncodingAndWrapping() {
  require(chart_readme::decode("\xef\xbb\xbfUTF-8").text == "UTF-8", "UTF-8 BOM removed");
  require(chart_readme::decode("\x82\xa0").text == "あ", "CP932 decoded");
  require(chart_readme::decode("\xc7\xd1\xb1\xdb", chart_readme::Encoding::Korean).text == "한글",
          "Korean legacy notes can override ambiguous Japanese bytes");
  require(chart_readme::decode("\xbc\xb3\xb8\xed").text == "설명", "Auto tries Korean after strict Japanese decoding fails");
  require(chart_readme::decode("\xc7\xd1", chart_readme::Encoding::Utf8).error == chart_readme::Error::Encoding,
          "explicit UTF-8 does not silently fall back to a legacy charset");
  require(chart_readme::decode(std::string("\xff\xfe\x5c\xd5", 4)).text == "한", "UTF-16 little endian decoded");
  require(chart_readme::decode(std::string("\xfe\xff\xd5\x5c", 4)).text == "한", "UTF-16 big endian decoded");
  require(chart_readme::decode(std::string("\xff\xfe\x00", 3)).error == chart_readme::Error::Encoding,
          "truncated UTF-16 rejected");
  require(chart_readme::decode(std::string("a\0b", 3)).error == chart_readme::Error::Encoding, "binary NUL not silently truncated");
  require(chart_readme::decode(std::string("\x82\xa0\0x", 4)).error == chart_readme::Error::Encoding,
          "CP932 binary NUL not silently truncated");
  std::size_t measuredBytes = 0;
  const std::string longLine(100000, 'x');
  const auto longWrapped = chart_readme::wrapLines(longLine, 40, [&](const std::string &line) {
    measuredBytes += line.size();
    return static_cast<int>(line.size());
  });
  require(measuredBytes < longLine.size() * 20, "long-line measurement scales with displayed content");
  require(longWrapped.size() == 2500, "long line wraps completely");
  const auto measure = [](const std::string &text) {
    std::vector<char32_t> scalars;
    require(asobmashow::text::decodeUtf8(text, scalars), "wrapped lines remain valid UTF-8");
    return static_cast<int>(scalars.size());
  };
  const auto wrapped = chart_readme::wrapLines("abc def\n\n한글abcd\n", 4, measure);
  require(wrapped == std::vector<std::string>{"abc ", "def", "", "한글ab", "cd", ""}, "wraps without dropping characters or blank lines");
}
}
int RunChartReadmeTests() {
  try { testDiscoveryAndReading(); testEncodingAndWrapping(); testLatestLoaderRequestWins(); }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
  return 0;
}
