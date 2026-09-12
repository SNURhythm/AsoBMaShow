#include "../src/archive/WindowsExtractionPathPolicy.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int checks = 0;
int failures = 0;

void expectComponent(std::string_view component, bool expected) {
  ++checks;
  if (archive_file::isSafeWindowsExtractionComponent(component) != expected) {
    ++failures;
    std::cerr << "FAIL component: " << component << " expected " << expected << '\n';
  }
}

void expectPath(std::string_view path, bool expected) {
  ++checks;
  if (archive_file::isSafeWindowsExtractionPath(path) != expected) {
    ++failures;
    std::cerr << "FAIL path: " << path << " expected " << expected << '\n';
  }
}

void testReservedDevicesAndAliases() {
  const std::string_view devices[] = {
      "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
      "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
      "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
      "COM\xC2\xB9", "COM\xC2\xB2", "COM\xC2\xB3",
      "LPT\xC2\xB9", "LPT\xC2\xB2", "LPT\xC2\xB3"};
  const std::string_view suffixes[] = {"", ".wav", ".tar.gz", " ", ".", " .wav", "  .tar.gz"};
  for (const auto device : devices) {
    std::string lowercase(device);
    for (char &character : lowercase) {
      if (character >= 'A' && character <= 'Z') {
        character += 'a' - 'A';
      }
    }
    for (const auto suffix : suffixes) {
      for (const auto &spelling : {std::string(device), lowercase}) {
        const auto alias = spelling + std::string(suffix);
        expectComponent(alias, false);
        expectPath("album/" + alias, false);
        expectPath("album/" + alias + "/song.wav", false);
        expectPath("album\\" + alias + "\\song.wav", false);
      }
    }
  }
  expectComponent("cOn.WaV", false);
  expectComponent("cOm\xC2\xB2 .wav", false);
  expectComponent("CoNiN$", false);
  expectComponent("cOnOuT$.wav", false);
}

void testInvalidCharactersAndNormalizationAliases() {
  for (unsigned int code = 0; code < 32; ++code) {
    const std::string invalid(1, static_cast<char>(code));
    expectComponent(invalid + "song.wav", false);
    expectComponent("so" + invalid + "ng.wav", false);
    expectComponent("song.wav" + invalid, false);
    expectPath("album/so" + invalid + "ng.wav", false);
    expectPath("al" + invalid + "bum/song.wav", false);
  }
  for (const char character : std::string_view("<>:\"/\\|?*")) {
    expectComponent("so" + std::string(1, character) + "ng.wav", false);
  }
  const std::string_view invalidComponents[] = {
      "", ".", "..", "...", " ", "song.", "song ", "song. ", "song .",
      " song.wav", "  NUL.wav", "song.wav:stream", "song.wav::$DATA", "AUX:stream"};
  for (const auto component : invalidComponents) {
    expectComponent(component, false);
  }
  const std::string_view invalidPaths[] = {
      "", ".", "..", "./song.wav", "../song.wav", "album/../song.wav",
      "album/./song.wav", "album//song.wav", "album\\\\song.wav",
      "/song.wav", "\\song.wav", "C:/song.wav", "C:song.wav",
      "//server/share/song.wav", "\\\\?\\C:\\song.wav", "\\\\.\\NUL",
      "album/", "album\\", "album./song.wav", "album /song.wav",
      " album/song.wav", "album/song.wav.", "album/song.wav ",
      "album/song.wav:stream", "album:stream/song.wav", "album/song.wav::$DATA",
      "album/so<ng.wav", "album/so>ng.wav", "album/so\"ng.wav", "album/so|ng.wav",
      "album/so?ng.wav", "album/so*ng.wav", "AUX/song.wav:stream", "NUL.wav"};
  for (const auto path : invalidPaths) {
    expectPath(path, false);
  }
}

void testOrdinaryNamesArePreserved() {
  const std::string_view validComponents[] = {
      "song.wav", "AUXiliary.wav", "AUX1", "AUX_", "AUX track.wav", "NULname.wav",
      "CONSOLE", "CONIN", "CONOUT", "CONIN$extra", "CONOUT$extra.wav", "PRNter",
      "COM", "COM0", "COM10", "COM11", "COM123", "COM1a.wav", "COM1_track.wav",
      "LPT", "LPT0", "LPT10", "LPT9a", ".hidden", ".NUL", "song..wav",
      "song .wav", "mix (01) [edit] + #1 & $100; ok!.wav", "AUX~1.wav",
      "\xED\x95\x9C\xEA\xB8\x80.wav", "\xE6\x9B\xB2\xE5\x90\x8D.wav",
      "caf\xC3\xA9.wav", "cafe\xCC\x81.wav", "\xF0\x9F\x8E\xB5.wav",
      "COM\xE2\x81\xB4", "LPT\xE2\x81\xB9", "COM\xEF\xBC\x91",
      "COM\xC2\xB9x", "LPT\xC2\xB2x", "AUX\xC2\xA0.wav",
      "\xE3\x80\x80song.wav", "song.wav\xE3\x80\x80", "song.wav\xC2\xA0"};
  for (const auto component : validComponents) {
    expectComponent(component, true);
    expectPath(component, true);
    expectPath("album/" + std::string(component) + "/song.wav", true);
    expectPath("album\\" + std::string(component) + "\\song.wav", true);
  }
  expectPath("album\\samples/kick.wav", true);
  const std::string surrounded = "!song.wav?";
  expectComponent(std::string_view(surrounded).substr(1, 8), true);
  expectPath(std::string_view(surrounded).substr(1, 8), true);
}

}

int main() {
  testReservedDevicesAndAliases();
  testInvalidCharactersAndNormalizationAliases();
  testOrdinaryNamesArePreserved();
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
