#include "ChartRepository.h"

#include "ChartRepositoryInternal.h"

chart_library::FolderClearDataByLongNoteMode
ChartRepository::Session::LoadFolderClearDataByLongNoteMode(
    const ScoreClearRankCache &scoreRanks) {
  return chart_repository_detail::LoadFolderClearDataByLongNoteMode(
      NativeHandleForScoreRepository(), scoreRanks);
}
