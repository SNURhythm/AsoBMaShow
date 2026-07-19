#pragma once

#include "ISkin.h"
#include "SkinTypes.h"

class DefaultSkin : public ISkin {
public:
  void buildLayout(const std::string &screenName, View *root,
                   void *data) override;
  bool rebuildLayoutSection(const std::string &sectionName, View *root,
                            void *data) override;

private:
  void buildResultLayout(View *root, ResultSkinData *data,
                         bool summaryOnly = false);
  void
  buildPresentationResultLayout(View *root, ResultSkinData *data,
                                const ResultPresentationModel &presentation,
                                bool summaryOnly,
                                bool authoritativePresentation);
  void buildResultSummary(View *root, ResultSkinData *data);
  void buildGameContext(View *root, void *data); // Example for future
};
