#include "scene/play/CompiledGameplayJudge.h"
#include "scene/play/Judge.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testCompiledJudgePreservesResolvedWindows() {
  Judge judge(1);
  judge.applyWindowScale(50, 200);
  const auto compiled = gameplay::CompiledGameplayJudge::from(judge);

  require(compiled.judgeAt(1'000'000, 1'010'000).judgement == PGreat,
          "compiled judge preserves the resolved PGreat window");
  require(compiled.judgeAt(1'000'000, 1'030'000).judgement == Great,
          "compiled judge preserves the resolved Great window");
  require(compiled.window(Bad)->lateMicros == 420'000,
          "compiled judge exposes the Bad late edge");
  require(compiled.latestHittableNoteTiming(1'000'000) == 1'500'000,
          "future cutoff uses the earliest hittable edge");
}
} // namespace

int main() {
  testCompiledJudgePreservesResolvedWindows();
  return 0;
}
