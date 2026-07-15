#include "ScoreRepository.h"

sqlite3 *ScoreRepository::NativeChartDatabase(
    ChartRepository::Session &chartSession) {
  return chartSession.NativeHandleForScoreRepository();
}

ScoreRepository::PreparedScoreQueryDatabase::PreparedScoreQueryDatabase(
    const ScoreRepository &repository, ChartRepository::Session &chartSession)
    : PreparedScoreQueryDatabase(
          repository, ScoreRepository::NativeChartDatabase(chartSession)) {}

ScoreRepository::PreparedScoreQueryDatabase
ScoreRepository::PrepareScoreQueryDatabase(
    ChartRepository::Session &chartSession) const {
  return PreparedScoreQueryDatabase(*this, chartSession);
}

ScoreClearRankCache ScoreRepository::LoadBestClearRanks(
    ChartRepository::Session &chartSession, std::string_view schema) {
  return LoadBestClearRanks(NativeChartDatabase(chartSession), schema);
}

ScoreBestCache ScoreRepository::LoadBestScores(
    ChartRepository::Session &chartSession, std::string_view schema) {
  return LoadBestScores(NativeChartDatabase(chartSession), schema);
}
