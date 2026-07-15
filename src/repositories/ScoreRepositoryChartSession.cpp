#include "ScoreRepository.h"
#include "ChartRepositoryInternal.h"
#include "ScoreRepositoryInternal.h"

std::unique_ptr<ScoreRepository::PreparedScoreQueryDatabase::State>
ScoreRepository::PrepareScoreQueryState(
    ChartRepository::Session &chartSession) const {
  return std::make_unique<PreparedScoreQueryDatabase::State>(
      *this, chartSession.impl_->database());
}

ScoreRepository::PreparedScoreQueryDatabase::PreparedScoreQueryDatabase(
    const ScoreRepository &repository, ChartRepository::Session &chartSession)
    : state_(repository.PrepareScoreQueryState(chartSession)) {}

ScoreRepository::PreparedScoreQueryDatabase
ScoreRepository::PrepareScoreQueryDatabase(
    ChartRepository::Session &chartSession) const {
  return PreparedScoreQueryDatabase(*this, chartSession);
}

ScoreClearRankCache ScoreRepository::LoadBestClearRanks(
    ChartRepository::Session &chartSession, std::string_view schema) {
  return score_repository_detail::LoadBestClearRanksOnConnection(
      chartSession.impl_->database(), schema);
}

ScoreBestCache ScoreRepository::LoadBestScores(
    ChartRepository::Session &chartSession, std::string_view schema) {
  return score_repository_detail::LoadBestScoresOnConnection(
      chartSession.impl_->database(), schema);
}
