#pragma once

#include "ISkin.h"
#include "SkinTypes.h"

class DefaultSkin : public ISkin {
public:
    void buildLayout(const std::string& screenName, View* root, void* data) override;

private:
    void buildResultLayout(View* root, ResultSkinData* data);
    void buildGameContext(View* root, void* data); // Example for future
};
