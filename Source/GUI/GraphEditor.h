// AuraPitch — NextDSP · AuraSuite
// GraphEditor.h : vue GRAPH EDITOR (étape 5B). Piano-roll calé sur la timeline du DAW : notes capturées (NoteBlob)
// en capsules dont l'épaisseur suit le niveau, courbe chantée en pointillés, courbe rejouée en trait lumineux, tête de
// lecture synchronisée.
//
// Souris :
//   * glisser une note (ou la sélection) verticalement : cible au demi-ton, aimantée sur la gamme ; Alt ou Maj : au cent ;
//   * rectangle (lasso) dans le vide : sélection ;
//   * double-clic : note quantifiée sur la gamme (0 c) ; Alt + double-clic ou clic droit : scission au curseur ;
//   * Cmd / Ctrl + glisser verticalement (ou la poignée du haut) : vibrato 0 … 200 % ;
//   * Cmd / Ctrl + Alt + glisser verticalement : formant de la note (±12 dt) ;
//   * poignée gauche, glisser horizontalement : glissé d'attaque depuis la note précédente (0 … 100 %) ;
//   * molette : défilement vertical ; Maj + molette ou molette horizontale : défilement dans le temps ;
//     Cmd / Ctrl + molette : zoom temps ; Cmd / Ctrl + Alt + molette : zoom hauteur ; boutons − / + ; FOLLOW.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../PluginProcessor.h"
#include "NextDSPLookAndFeel.h"

namespace nextdsp
{
class GraphEditor final : public juce::Component
{
public:
    explicit GraphEditor (AuraPitchProcessor& processor);

    /** Timer de l'éditeur : suivi de la lecture, rafraîchissement. */
    void tick (double dtSeconds);
    void setScale (std::uint16_t activeMask, int tonic) { mask = activeMask; key = tonic; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove (const juce::MouseEvent&) override;

    //--- vue (publique pour le banc d'essai) ------------------------------------------------------------------------
    juce::Rectangle<float> plotArea() const;
    float xForTime (double t) const;
    double timeForX (float x) const;
    float yForMidi (double midi) const;
    double midiForY (float y) const;
    float rowHeight() const;
    void showRange (double startSec, double spanSec, double centreMidi, double semitones);
    void fitToNotes();
    double getViewStart() const noexcept { return viewStart; }
    double getViewSpan() const noexcept  { return viewSpan; }
    double getSemitonesVisible() const noexcept { return semitones; }
    const std::vector<bool>& getSelection() const noexcept { return selected; }

    juce::TextButton captureButton { "CAPTURE" }, clearButton { "CLEAR" }, quantizeButton { "QUANTIZE ALL" },
                     followButton { "FOLLOW" }, zoomOut { "-" }, zoomIn { "+" };

private:
    enum class Drag { None, Pitch, Vibrato, Formant, Transition, Lasso };
    enum class Part { None, Body, LeftHandle, TopHandle };

    int noteAt (juce::Point<float> p, Part& part) const;
    juce::Rectangle<float> noteBounds (int index) const;
    void edited();
    void syncSelection();
    void zoomTime (double factor, double anchorTime);
    void zoomPitch (double factor);

    AuraPitchProcessor& processor;
    std::uint16_t mask = 0x0fff;
    int key = 0;
    double viewStart = 0.0, viewSpan = 8.0, centre = 60.0, semitones = 18.0;
    bool userMoved = false;

    std::vector<bool> selected;
    Drag drag = Drag::None;
    int dragNote = -1;
    juce::Point<float> dragStart;
    juce::Rectangle<float> lasso;
    std::vector<double> dragOrigin;          // valeur de départ du paramètre glissé, par note
    int hoverNote = -1;
    Part hoverPart = Part::None;
    double lastPlayhead = 0.0;
};
}
