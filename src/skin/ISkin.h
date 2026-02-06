#pragma once

#include <string>
#include "../view/View.h"

class ISkin {
public:
    virtual ~ISkin() = default;
    
    /**
     * Builds the layout for a named screen.
     * @param screenName The name of the screen (e.g. "Result", "MainMenu").
     * @param root The root view to add children to.
     * @param data Pointer to a screen-specific data structure (e.g. ResultSkinData*).
     */
    virtual void buildLayout(const std::string& screenName, View* root, void* data) = 0;
};
