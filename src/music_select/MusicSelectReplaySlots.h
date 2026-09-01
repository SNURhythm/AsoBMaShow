#pragma once

#include "../replay/BeatorajaReplayPath.h"
#include "../repositories/ChartRepository.h"
#include "../repositories/ReplayRepository.h"

#include <array>
#include <filesystem>
#include <optional>
#include <span>

[[nodiscard]] std::optional<std::array<replay::ReplayPathIdentity, 4>>
musicSelectChartReplaySlotPaths(const ChartMetaRecord &record,
                                int selectedLongNoteMode);

[[nodiscard]] std::array<bool, 4> musicSelectExistingChartReplaySlots(
    const ChartMetaRecord &record, int selectedLongNoteMode,
    const std::filesystem::path &profileRoot);

[[nodiscard]] std::optional<int> musicSelectChartReplayResultId(
    std::span<const ModernReplayFileInventoryEntry> entries,
    const replay::ReplayPathIdentity &slot);
