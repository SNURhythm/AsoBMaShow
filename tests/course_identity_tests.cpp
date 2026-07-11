#include "CourseIdentity.h"
#include "CoursePlaySession.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

constexpr std::string_view kShaA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kShaB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view kMd5A = "11111111111111111111111111111111";
constexpr std::string_view kMd5B = "22222222222222222222222222222222";

course_identity::ChartIdentity shaChart(std::string_view sha256,
                                        std::string_view md5 = {}) {
  return {.sha256 = std::string(sha256), .md5 = std::string(md5)};
}

course_identity::ChartIdentity md5Chart(std::string_view md5) {
  return {.md5 = std::string(md5)};
}

void testCanonicalKeyUsesOnlyDurableSemanticIdentity() {
  using namespace course_identity;
  const std::vector<ChartIdentity> charts = {shaChart(kShaA, kMd5A),
                                             md5Chart(kMd5B)};
  std::vector<ChartIdentity> reversed = charts;
  std::ranges::reverse(reversed);

  const std::string canonical =
      makeCourseKey(charts, R"(["no_speed","gauge_7k"])");
  expect(canonical.starts_with("course:v1:") && canonical.size() == 74,
         "course key is a versioned SHA-256 digest");
  expect(canonical == makeCourseKey(charts, R"(["gauge-7k","NO SPEED"])"),
         "constraint formatting and order are canonical");
  expect(makeCourseKey(charts, "[]") ==
             makeCourseKey(charts, R"(["grade","unknown"])"),
         "grade and unknown restrictions are not content identity");
  expect(makeCourseKey(charts, "[]") != makeCourseKey(reversed, "[]"),
         "chart order is content identity");
  expect(makeCourseKey(charts, R"(["gauge_7k"])") !=
             makeCourseKey(charts, R"(["gauge_9k"])"),
         "score-affecting constraints are content identity");

  const std::vector<ChartIdentity> normalizedCharts = {
      shaChart(
          "  AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA "
          " ",
          "11111111111111111111111111111111"),
      md5Chart("  22222222222222222222222222222222  ")};
  expect(canonical ==
             makeCourseKey(normalizedCharts, R"([" gauge_7k "," no-speed "])"),
         "hash and constraint spelling differences normalize");

  const std::vector<ChartIdentity> changedSecondaryHash = {
      shaChart(kShaA, kMd5B), md5Chart(kMd5B)};
  expect(canonical ==
             makeCourseKey(changedSecondaryHash, R"(["no_speed","gauge_7k"])"),
         "SHA-256 is authoritative when available");
}

void testMalformedChartIdentityCannotProduceDurableKey() {
  using namespace course_identity;
  expect(makeCourseKey({}, "[]").empty(),
         "an empty chart sequence has no course identity");
  expect(makeCourseKey(std::vector<ChartIdentity>{{}}, "[]").empty(),
         "an empty chart identity is rejected");
  expect(makeCourseKey(std::vector<ChartIdentity>{shaChart("not-a-hash")}, "[]")
             .empty(),
         "a malformed SHA-256 is rejected");
  expect(
      makeCourseKey(std::vector<ChartIdentity>{md5Chart("abc")}, "[]").empty(),
      "a malformed MD5 is rejected");
  expect(makeCourseKey(
             std::vector<ChartIdentity>{shaChart(kShaA, "invalid-md5")}, "[]")
             .empty(),
         "a malformed secondary hash is rejected");
}

void testSessionAndLegacyKeysShareTheCanonicalDefinition() {
  using namespace course_identity;
  const std::vector<ChartIdentity> charts = {shaChart(kShaA), md5Chart(kMd5B)};
  constexpr std::string_view constraints =
      R"(["no_speed","gauge_7k","grade_mirror"] )";

  CoursePlaySession session;
  session.courseName = "Renamable display name";
  session.constraintJson = constraints;
  session.entries.resize(2);
  session.entries[0].meta.SHA256 = std::string(kShaA);
  session.entries[0].meta.BmsPath = "/path/must/not/be/identity.bms";
  session.entries[1].meta.MD5 = std::string(kMd5B);
  session.entries[1].meta.BmsPath = "/another/path.bms";
  expect(makeCourseKey(session) == makeCourseKey(charts, constraints),
         "session identity uses hashes and constraints without paths or name");

  const std::string legacyNamedKey =
      "course:Old display name\nconstraint:" + std::string(constraints) +
      "\nsha256:" + std::string(kShaA) + "\nmd5:" + std::string(kMd5B) + "\n";
  const auto parsed = parseLegacyScoreKey(legacyNamedKey);
  expect(parsed.has_value(), "well-formed legacy score key parses");
  if (parsed) {
    expect(parsed->courseName == "Old display name",
           "legacy display name remains available as evidence");
    expect(parsed->charts.size() == charts.size(),
           "legacy ordered chart identities parse");
    expect(parsed->courseKey == makeCourseKey(charts, constraints),
           "legacy score key drops course name");
  }

  expect(!parseLegacyScoreKey("course:x\nconstraint:[]\npath:/old/a.bms\n"),
         "legacy paths cannot become durable identity");
  expect(!parseLegacyScoreKey("course:x\nconstraint:[]\nsha256:bad\n"),
         "malformed legacy hashes remain unconverted");
  expect(!parseLegacyScoreKey("course:x\nconstraint:[]\n"),
         "legacy keys without charts remain unconverted");
}

void testDefinitionMatchingUsesStrongestCommonHashes() {
  using namespace course_identity;
  const ChartIdentity md5Only = md5Chart(kMd5A);
  const ChartIdentity enriched = shaChart(kShaA, kMd5A);
  expect(sameChart(md5Only, enriched),
         "MD5 bridges an identity when SHA-256 is not common");
  expect(!sameChart(shaChart(kShaA, kMd5A), shaChart(kShaB, kMd5A)),
         "mismatching common SHA-256 overrides matching MD5");
  expect(!sameChart(md5Only, md5Chart(kMd5B)),
         "different common MD5 identities do not match");

  const Definition before{
      .courseId = 12,
      .courseKey = "course:v1:old",
      .name = "Old name",
      .groupName = "Old group",
      .constraintJson = R"(["gauge_7k","no_speed"])",
      .charts = {md5Only, md5Chart(kMd5B)},
  };
  Definition refreshed{
      .courseId = 99,
      .courseKey = "course:v1:new",
      .name = "New name",
      .groupName = "New group",
      .constraintJson = R"(["NO SPEED","gauge-7k"])",
      .charts = {enriched, md5Chart(kMd5B)},
  };
  expect(sameDefinition(before, refreshed),
         "definition matching ignores navigation and display metadata");
  refreshed.constraintJson = R"(["no_speed","gauge_9k"])";
  expect(!sameDefinition(before, refreshed),
         "definition matching includes semantic constraints");
  expect(!sameDefinition(Definition{}, Definition{}),
         "empty definitions never match as durable identity");

  const std::vector<ChartIdentity> prefix = {enriched};
  expect(prefixMatches(prefix, before.charts),
         "ordered strongest-common chart prefix matches");
  expect(!prefixMatches(before.charts, prefix),
         "a longer recorded sequence is not a prefix");
  expect(!prefixMatches(std::vector<ChartIdentity>{shaChart(kShaB, kMd5B)},
                        before.charts),
         "a mismatching strongest common prefix is rejected");
}

} // namespace

int main() {
  testCanonicalKeyUsesOnlyDurableSemanticIdentity();
  testMalformedChartIdentityCannotProduceDurableKey();
  testSessionAndLegacyKeysShareTheCanonicalDefinition();
  testDefinitionMatchingUsesStrongestCommonHashes();

  if (failures != 0) {
    std::cerr << failures << " course identity test(s) failed.\n";
    return 1;
  }
  std::cout << "course identity tests passed\n";
  return 0;
}
