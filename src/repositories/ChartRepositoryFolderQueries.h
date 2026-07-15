#pragma once

#include "../LibraryFolderClearData.h"
#include "ScoreRepositoryModels.h"
#include "../sqlite3.h"

namespace chart_repository_detail {

chart_library::FolderClearDataByLongNoteMode
LoadFolderClearDataByLongNoteMode(sqlite3 *database,
                                  const ScoreClearRankCache &scoreRanks);

} // namespace chart_repository_detail
