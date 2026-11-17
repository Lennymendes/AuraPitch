// AuraPitch — NextDSP · AuraSuite
// GraphWindow.h : vue agrandie du GRAPH EDITOR (étape 6).
//
// Une fenêtre de bureau dédiée (1200 × 800 par défaut, redimensionnable, bouton plein écran de l'OS) reçoit le
// composant GraphEditor de l'éditeur principal — le même objet, pas une copie : la vue, la sélection et le zoom sont
// conservés à l'aller comme au retour. La fenêtre ne possède pas le GraphEditor ; elle le rend à l'éditeur quand on la
// ferme (bouton de fermeture, Échap, ou bouton réduire de l'en-tête).
//
// Interface seulement : aucune donnée partagée avec le thread audio n'est ajoutée (le graphe est toujours publié par
// AuraPitchProcessor::graphEdited, sans verrou côté audio).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "GraphEditor.h"

namespace nextdsp
{
/** Panneau affiché à la place du GRAPH EDITOR dans l'éditeur pendant qu'il est dans sa fenêtre. */
class GraphDetachedPanel final : public juce::Component
{
public:
    GraphDetachedPanel();
    void paint (juce::Graphics&) override;
    void resized() override;
    juce::TextButton showButton { "SHOW WINDOW" }, returnButton { "RETURN HERE" };
};

class GraphWindow final : public juce::DocumentWindow
{
public:
    static constexpr int kDefaultWidth = 1200, kDefaultHeight = 800;

    /** `bounds` vides : 1200 × 800 centré sur l'écran de `near`. `onClose` est appelé (de façon asynchrone par
        l'éditeur) quand l'utilisateur ferme la fenêtre. */
    GraphWindow (GraphEditor& editor, juce::LookAndFeel& lnf, juce::Rectangle<int> bounds,
                 const juce::Component* near, std::function<void()> onClose);
    ~GraphWindow() override;

    void closeButtonPressed() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class Holder;
    std::unique_ptr<Holder> holder;
    std::function<void()> onClose;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphWindow)
};
}
