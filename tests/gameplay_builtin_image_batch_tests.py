import os
import pathlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch


root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "src/scene/play/GamePlayScene.cpp").read_text(encoding="utf-8")
start = source.index("[](const std::map<int, std::filesystem::path> &paths,",
                     source.index(".builtinImageBatchReader ="))
end = source.index(",\n          .liveResourceCounters", start)
callback = source[start:end]
fixture = r'''
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stop_token>
#include <string>
#include <type_traits>
#include <vector>
namespace skin {
struct SkinBuiltinImageBatch { int reference; std::vector<unsigned char> bytes; };
}
namespace archive_file {
namespace fs = std::filesystem;
struct FileData { fs::path path; std::vector<unsigned char> bytes; };
std::size_t unboundedReads = 0;
std::size_t peakPayload = 0;
std::vector<std::size_t> limits;
bool splitVirtualPath(const fs::path &path, fs::path &archive, fs::path &inner) {
  const auto text = path.string();
  const auto split = text.find('#');
  if (split == std::string::npos) return false;
  archive = text.substr(0, split);
  inner = text.substr(split + 1);
  return true;
}
fs::path makeVirtualPath(const fs::path &archive, const fs::path &inner) {
  return archive.string() + "#" + inner.string();
}
std::size_t size(const fs::path &path) {
  return path.string().find("big") == std::string::npos ? 8 : 256;
}
bool readFileBounded(const fs::path &path, std::vector<unsigned char> &bytes,
                     std::size_t maximum, std::string *, std::stop_token stop) {
  limits.push_back(maximum);
  if (stop.stop_requested() || size(path) > maximum) return false;
  bytes.resize(size(path));
  peakPayload = std::max(peakPayload, bytes.size());
  return true;
}
bool readArchiveEntries(const fs::path &, const std::vector<fs::path> &paths,
                        std::vector<FileData> &files, std::string *,
                        const std::function<bool()> &progress) {
  if (!progress()) return false;
  ++unboundedReads;
  for (const auto &path : paths) {
    if (path == "fallback") continue;
    files.push_back({path, std::vector<unsigned char>(size(path))});
    peakPayload = std::max(peakPayload, files.back().bytes.size());
  }
  return true;
}
}
template <typename Reader>
bool invoke(Reader reader, const std::map<int, std::filesystem::path> &paths,
             std::vector<skin::SkinBuiltinImageBatch> &output,
             std::size_t limit, std::stop_token stop = {}) {
  if constexpr (std::is_invocable_v<Reader, decltype(paths), decltype(output),
                                     std::size_t, std::stop_token>) {
    return reader(paths, output, limit, stop);
  } else {
    return reader(paths, output, stop);
  }
}
int main() {
  const auto reader = CALLBACK;
  std::vector<skin::SkinBuiltinImageBatch> output;
  const std::map<int, std::filesystem::path> paths{
      {100, "pack.zip#big"}, {101, "pack.zip#small"}, {102, "plain-small"},
      {103, "plain-big"}, {104, "pack.zip#fallback"}};
  assert(invoke(reader, paths, output, 32));
  assert(archive_file::unboundedReads == 0 && archive_file::peakPayload <= 32);
  assert(output.size() == 3);
  for (const auto &item : output) assert(item.bytes.size() == 8);
  assert(archive_file::limits.size() == 5);
  for (const auto limit : archive_file::limits) assert(limit == 32);
  output.clear();
  archive_file::limits.clear();
  assert(invoke(reader, paths, output, std::numeric_limits<std::size_t>::max()));
  assert(output.size() == 5 && archive_file::unboundedReads == 1);
  for (const auto limit : archive_file::limits)
    assert(limit == std::numeric_limits<std::size_t>::max());
  std::stop_source stop;
  stop.request_stop();
  output.clear();
  archive_file::limits.clear();
  invoke(reader, paths, output, 32, stop.get_token());
  assert(output.empty() && archive_file::limits.empty());
  std::cout << "bounded batch: zero unbounded reads, peak <=32; small, fallback, unrestricted, cancellation passed\n";
}
'''
def run_fixture():
    compiler = os.environ.get("ASOBMASHOW_TEST_CXX_COMPILER", "c++")
    frontend = os.environ.get("ASOBMASHOW_TEST_CXX_FRONTEND_VARIANT", "")
    compiler_id = os.environ.get("ASOBMASHOW_TEST_CXX_COMPILER_ID", "")
    msvc = frontend == "MSVC" or compiler_id == "MSVC"
    with tempfile.TemporaryDirectory(prefix="gameplay-builtin-batch-") as temporary:
        folder = pathlib.Path(temporary)
        translation_unit = folder / "batch.cpp"
        translation_unit.write_text(fixture.replace("CALLBACK", callback), encoding="utf-8")
        executable = folder / ("batch.exe" if os.name == "nt" or msvc else "batch")
        if msvc:
            command = [compiler, "/nologo", "/std:c++latest", "/EHsc",
                       str(translation_unit), f"/Fo{translation_unit.with_suffix('.obj')}",
                       f"/Fe{executable}"]
        else:
            command = [compiler, "-std=c++23", "-pthread", str(translation_unit),
                       "-o", str(executable)]
        subprocess.run(command, cwd=folder, check=True)
        subprocess.run([str(executable)], cwd=folder, check=True)


class GameplayBuiltinImageBatchTests(unittest.TestCase):
    def test_bounded_unrestricted_and_cancelled_batches(self):
        run_fixture()

    def test_fixture_uses_configured_compiler_frontend(self):
        for frontend, compiler_id, standard in (
            ("GNU", "Clang", "-std=c++23"),
            ("MSVC", "MSVC", "/std:c++latest"),
            ("MSVC", "Clang", "/std:c++latest"),
            ("", "MSVC", "/std:c++latest"),
        ):
            with self.subTest(frontend=frontend, compiler_id=compiler_id):
                with patch.dict(os.environ, {
                    "ASOBMASHOW_TEST_CXX_COMPILER": "/configured/compiler",
                    "ASOBMASHOW_TEST_CXX_FRONTEND_VARIANT": frontend,
                    "ASOBMASHOW_TEST_CXX_COMPILER_ID": compiler_id,
                }), patch.object(subprocess, "run", return_value=
                    subprocess.CompletedProcess([], 0)) as run:
                    run_fixture()
                    command = run.call_args_list[0].args[0]
                    self.assertEqual(command[0], "/configured/compiler")
                    self.assertIn(standard, command)
                    if frontend == "MSVC" or compiler_id == "MSVC":
                        self.assertNotIn("-pthread", command)
                        self.assertNotIn("-o", command)
                        self.assertTrue(any(flag.startswith("/Fo") for flag in command))
                        self.assertTrue(any(flag.startswith("/Fe") and flag.endswith(".exe")
                                            for flag in command))
                    self.assertEqual(run.call_args_list[1].kwargs["cwd"],
                                     run.call_args_list[0].kwargs["cwd"])


if __name__ == "__main__":
    unittest.main()
