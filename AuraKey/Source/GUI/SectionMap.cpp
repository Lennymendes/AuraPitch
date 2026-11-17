// AuraKey — NextDSP · AuraSuite
#include "SectionMap.h"

#include "../../../Source/GUI/NextDSPLookAndFeel.h"

namespace nextdsp
{
using namespace juce;
using namespace aura::key;

String formatTime (double s)
{
    const int total = (int) std::floor (jmax (0.0, s) + 0.5);
    return String (total / 60) + ":" + String (total % 60).paddedLeft ('0', 2);
}

void SectionMap::setSections (const std::vector<Section>& s, double durationSec)
{
    auto near = [] (double x, double y) { return std::abs (x - y) < 1.0e-9; };
    const bool same = near (durationSec, duration) && s.size() == sections.size()
                      && std::equal (s.begin(), s.end(), sections.begin(), [&] (const Section& a, const Section& b)
                         { return near (a.start, b.start) && near (a.end, b.end) && a.tonic == b.tonic && a.minor == b.minor; });
    if (same) return;
    sections = s;
    duration = durationSec;
    hovered = -1;
    repaint();
}

Rectangle<float> SectionMap::barArea() const
{
    return getLocalBounds().toFloat().withTrimmedTop (18.0f).withTrimmedBottom (4.0f);
}

float SectionMap::xForTime (double t) const
{
    const auto a = barArea();
    return duration > 0.0 ? a.getX() + (float) (t / duration) * a.getWidth() : a.getX();
}

int SectionMap::sectionAt (Point<float> p) const
{
    if (! barArea().contains (p)) return -1;
    for (size_t i = 0; i < sections.size(); ++i)
        if (p.x >= xForTime (sections[i].start) && p.x < xForTime (sections[i].end) + 0.5f)
            return (int) i;
    return -1;
}

String SectionMap::getHoverText() const
{
    if (hovered < 0 || hovered >= (int) sections.size()) return {};
    const auto& s = sections[(size_t) hovered];
    return formatTime (s.start) + String (CharPointer_UTF8 (" \xe2\x80\x93 ")) + formatTime (s.end) + String (CharPointer_UTF8 ("  \xc2\xb7  "))
           + keyName (s.tonic, s.minor) + "  " + camelot (s.tonic, s.minor) + String (CharPointer_UTF8 ("  \xc2\xb7  "))
           + String (roundToInt (100.0 * s.confidence)) + " %";
}

void SectionMap::paint (Graphics& g)
{
    const auto a = barArea();
    // règle
    g.setFont (monoFont (9.5f));
    if (duration > 0.0)
    {
        double step = 5.0;
        for (double s : { 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 })
            if (duration / s <= jmax (4.0, (double) a.getWidth() / 70.0)) { step = s; break; }
        for (double t = 0.0; t <= duration + 1e-9; t += step)
        {
            const float x = xForTime (t);
            g.setColour (colours::grid.brighter (0.25f));
            g.drawVerticalLine (roundToInt (x), a.getY() - 4.0f, a.getY());
            g.setColour (colours::textFaint);
            if (x < a.getRight() - 30.0f)
                g.drawText (formatTime (t), Rectangle<float> (x + 3.0f, 0.0f, 50.0f, 14.0f), Justification::centredLeft);
        }
    }
    g.setColour (colours::panelHi);
    g.fillRoundedRectangle (a, 5.0f);
    if (sections.empty())
    {
        g.setFont (font (11.5f));
        g.setColour (colours::textDim);
        g.drawText ("Import a file to map its major and minor sections", a, Justification::centred);
        return;
    }
    for (size_t i = 0; i < sections.size(); ++i)
    {
        const auto& s = sections[i];
        auto r = Rectangle<float> (xForTime (s.start), a.getY(), xForTime (s.end) - xForTime (s.start), a.getHeight()).reduced (1.0f, 0.0f);
        const auto accent = s.minor ? colours::violet : colours::cyan;
        const int k = keyIndex (s.tonic, s.minor);
        const bool lit = (int) i == hovered || k == highlight;
        g.setColour (accent.withAlpha ((lit ? 0.42f : 0.22f) + 0.25f * (float) s.confidence));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (accent.withAlpha (lit ? 1.0f : 0.7f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, lit ? 1.6f : 1.0f);
        if (r.getWidth() > 46.0f)
        {
            auto t = r.reduced (8.0f, 4.0f);
            g.setColour (colours::text);
            g.setFont (font (jlimit (11.0f, 15.0f, a.getHeight() * 0.3f), true));
            g.drawText (r.getWidth() > 120.0f ? String (keyName (s.tonic, s.minor)) : String (keyShortName (s.tonic, s.minor)),
                        t.removeFromTop (t.getHeight() * 0.55f), Justification::bottomLeft, true);
            g.setFont (monoFont (10.0f));
            g.setColour (colours::text.withAlpha (0.7f));
            g.drawText (String (camelot (s.tonic, s.minor)) + "  " + String (roundToInt (100.0 * s.confidence)) + " %", t, Justification::topLeft, true);
        }
    }
}

void SectionMap::mouseMove (const MouseEvent& e)
{
    const int i = sectionAt (e.position);
    if (i != hovered)
    {
        hovered = i;
        setMouseCursor (i >= 0 ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
        repaint();
        if (auto* p = getParentComponent()) p->repaint();
    }
}

void SectionMap::mouseExit (const MouseEvent&)
{
    if (hovered >= 0)
    {
        hovered = -1;
        repaint();
        if (auto* p = getParentComponent()) p->repaint();
    }
}

void SectionMap::mouseDown (const MouseEvent& e)
{
    const int i = sectionAt (e.position);
    if (i >= 0 && onSectionClicked)
        onSectionClicked (keyIndex (sections[(size_t) i].tonic, sections[(size_t) i].minor));
}
}
