#pragma once

// Captured by compiling the parser pair from immutable app revision
// 68382de1627f321aa8a56c7a961d75b4f7b974b7, seed 1. See adjacent README.
namespace pms_scan_baseline {
inline constexpr char chart[] =
    "#TITLE PMS lane mapping\n#BPM 120\n#WAV01 note.wav\n"
    "#00011:01\n#00022:01\n#00025:01\n#00152:0101\n";
inline constexpr char sha256[] =
    "ab3ec05940186d15534825fadef2a560e08e8d16fa7c24dbe7fe1af22eb52b82";
inline constexpr int ordinaryKeys = 9;
inline constexpr int archivedKeys = 10;
inline constexpr int notes = 4;
inline constexpr int longNotes = 1;
inline constexpr long long playLength = 3000000;
inline constexpr long long totalLength = 4000000;
} // namespace pms_scan_baseline
