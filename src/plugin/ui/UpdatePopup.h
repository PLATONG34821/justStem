#pragma once

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace juststem::ui
{
/** Overlay card announcing a new release: "justStem 0.0.3 is out", with Later and Download.
    Clicking outside the card is the same as Later. */
class UpdatePopup final : public juce::Component
{
public:
    explicit UpdatePopup (const juce::String& version);

    std::function<void()> onLater, onDownload;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::staticText);
    }

private:
    juce::Rectangle<int> card() const;

    const juce::String version;
    PillButton laterButton { "Later" };
    PillButton downloadButton { "Download", icons::download() };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdatePopup)
};
} // namespace juststem::ui
