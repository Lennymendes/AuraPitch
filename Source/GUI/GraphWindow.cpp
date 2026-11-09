// AuraPitch — NextDSP · AuraSuite
#include "GraphWindow.h"

namespace nextdsp
{
using namespace juce;

GraphDetachedPanel::GraphDetachedPanel()
{
    showButton.setTooltip ("Bring the GRAPH EDITOR window to the front");
    returnButton.setTooltip ("Close the large window and edit here");
    addAndMakeVisible (showButton);
    addAndMakeVisible (returnButton);
}

void GraphDetachedPanel::paint (Graphics& g)
{
    drawPanel (g, getLocalBounds().toFloat());
    auto r = getLocalBounds().toFloat();
    g.setFont (font (11.0f, true));
    g.setColour (colours::textDim);
    g.drawText ("GRAPH EDITOR", r.reduced (14.0f, 8.0f).removeFromTop (16.0f), Justification::centredLeft);
    auto c = r.withSizeKeepingCentre (r.getWidth(), 60.0f).translated (0.0f, -24.0f);
    g.setFont (font (15.0f, true));
    g.setColour (colours::cyan.withAlpha (0.9f));
    g.drawText ("Open in the expanded window", c.removeFromTop (24.0f), Justification::centred);
    g.setFont (font (12.0f));
    g.setColour (colours::textDim);
    g.drawText ("Notes, selection and zoom follow the editor back when the window closes", c, Justification::centred);
}

void GraphDetachedPanel::resized()
{
    auto b = getLocalBounds().withSizeKeepingCentre (2 * 128 + 10, 30).translated (0, 34);
    showButton.setBounds (b.removeFromLeft (128));
    b.removeFromLeft (10);
    returnButton.setBounds (b);
}

/** Contenu de la fenêtre : fond NextDSP, GraphEditor avec une marge, info-bulles propres à cette fenêtre (celles de
    l'éditeur principal ne s'affichent que dans sa propre fenêtre). */
class GraphWindow::Holder final : public Component
{
public:
    explicit Holder (GraphEditor& e) : editor (e)
    {
        setOpaque (true);
        addAndMakeVisible (editor);
    }
    ~Holder() override { removeChildComponent (&editor); }

    void paint (Graphics& g) override { g.fillAll (colours::background); }
    void resized() override { editor.setBounds (getLocalBounds().reduced (10)); }

private:
    GraphEditor& editor;
    TooltipWindow tooltips { this, 600 };
};

GraphWindow::GraphWindow (GraphEditor& editor, juce::LookAndFeel& lnf, Rectangle<int> bounds, const Component* near,
                          std::function<void()> closeCallback)
    : DocumentWindow ("AuraPitch — Graph Editor", colours::background, DocumentWindow::allButtons, false),
      onClose (std::move (closeCallback))
{
    setLookAndFeel (&lnf);
    holder = std::make_unique<Holder> (editor);
    setUsingNativeTitleBar (true);
    setContentNonOwned (holder.get(), false);
    setResizable (true, false);
    setResizeLimits (640, 400, 16384, 16384);
    setWantsKeyboardFocus (true);

    if (bounds.isEmpty())
    {
        const auto& displays = Desktop::getInstance().getDisplays();
        const auto* d = near != nullptr && near->isShowing() ? displays.getDisplayForRect (near->getScreenBounds())
                                                             : displays.getPrimaryDisplay();
        const auto area = d != nullptr ? d->userBounds.toNearestInt() : Rectangle<int> (0, 0, kDefaultWidth + 80, kDefaultHeight + 80);
        bounds = area.withSizeKeepingCentre (jmin (kDefaultWidth, area.getWidth() - 40),
                                             jmin (kDefaultHeight, area.getHeight() - 40));
    }
    setBounds (bounds);
#if JUCE_MAC
    setAlwaysOnTop (true);       // reste devant la fenêtre du plug-in, souvent flottante dans les DAW macOS
#endif
    addToDesktop();
    setVisible (true);
    toFront (true);
    editor.setDetached (true);
}

GraphWindow::~GraphWindow()
{
    clearContentComponent();
    holder.reset();
    setLookAndFeel (nullptr);
}

void GraphWindow::closeButtonPressed()
{
    if (onClose) onClose();
}

bool GraphWindow::keyPressed (const KeyPress& k)
{
    if (k == KeyPress::escapeKey) { closeButtonPressed(); return true; }
    return DocumentWindow::keyPressed (k);
}
}
