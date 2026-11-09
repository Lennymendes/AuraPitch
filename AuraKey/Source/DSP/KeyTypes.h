// AuraKey — NextDSP · AuraSuite
// KeyTypes.h : noms des tonalités, code Camelot, position sur le cercle des quintes, résultats d'analyse.
#pragma once

#include <array>
#include <string>
#include <vector>

namespace aura::key
{

/** Débit d'évidence du modèle bayésien de l'Auto-Key (étape 5A) pour un chromagramme polyphonique : une trame de
    chroma porte plusieurs notes, mais elles ne sont pas indépendantes d'une trame à l'autre (accords tenus). Réglé pour
    qu'une progression tonale nette dépasse 90 % en 10 à 20 s d'écoute. */
inline constexpr double kChromaEvidenceRate = 1.5;

/** Index de clé : 0 … 11 = C … B majeur, 12 … 23 = C … B mineur. */
inline int keyIndex (int tonic, bool minor) noexcept { return (minor ? 12 : 0) + ((tonic % 12) + 12) % 12; }

inline const char* pitchName (int pc) noexcept
{
    // orthographe usuelle : dièses pour F#, C#, G# ; bémols pour Eb, Ab, Bb
    static constexpr const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    return names[((pc % 12) + 12) % 12];
}

/** Nom affiché : orthographe de la gamme (Db majeur, C# mineur, F# majeur / mineur, Bb mineur…). */
inline std::string keyName (int tonic, bool minor)
{
    tonic = ((tonic % 12) + 12) % 12;
    static constexpr const char* majorNames[] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    static constexpr const char* minorNames[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B" };
    return std::string (minor ? minorNames[tonic] : majorNames[tonic]) + (minor ? " Minor" : " Major");
}

inline std::string keyShortName (int tonic, bool minor)
{
    tonic = ((tonic % 12) + 12) % 12;
    static constexpr const char* majorNames[] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    static constexpr const char* minorNames[] = { "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "Bbm", "Bm" };
    return minor ? minorNames[tonic] : majorNames[tonic];
}

/** Position sur le cercle des quintes (0 = C / Am en haut, sens horaire : G / Em, D / Bm…). */
inline int fifthsPosition (int tonic, bool minor) noexcept
{
    const int major = minor ? (tonic + 3) % 12 : tonic;          // relative majeure
    return (major * 7) % 12;
}

/** Tonique (classe de hauteur) de la case `position` du cercle. */
inline int tonicAtFifthsPosition (int position, bool minor) noexcept
{
    const int major = (((position % 12) + 12) % 12 * 7) % 12;
    return minor ? (major + 9) % 12 : major;
}

/** Code Camelot (roue des DJ) : 8B = C majeur, 8A = A mineur ; +1 par quinte. */
inline std::string camelot (int tonic, bool minor)
{
    const int pos = fifthsPosition (tonic, minor);                // 0 = C / Am
    const int number = (pos + 7) % 12 + 1;                        // C -> 8
    return std::to_string (number) + (minor ? "A" : "B");
}

/** Classes de hauteur de la gamme (majeure naturelle, ou mineure naturelle) : masque 12 bits. */
inline unsigned scaleMask (int tonic, bool minor) noexcept
{
    static constexpr int major[] = { 0, 2, 4, 5, 7, 9, 11 }, minorSteps[] = { 0, 2, 3, 5, 7, 8, 10 };
    unsigned m = 0;
    for (int i = 0; i < 7; ++i)
        m |= 1u << ((tonic + (minor ? minorSteps[i] : major[i])) % 12);
    return m;
}

struct Section
{
    double start = 0.0, end = 0.0;    // secondes depuis le début du fichier
    int tonic = 0;
    bool minor = false;
    double confidence = 0.0;          // probabilité a posteriori moyenne de la clé sur la section
};

struct KeyAnalysisResult
{
    bool valid = false;
    int tonic = -1;
    bool minor = false;
    double confidence = 0.0;
    std::array<double, 24> posteriors {};
    std::array<double, 12> chroma {};       // chromagramme moyen (somme 1)
    double bpm = 0.0, bpmConfidence = 0.0;
    double tuningCents = 0.0;               // accord estimé du morceau par rapport à A = 440 Hz
    double durationSec = 0.0;
    double tonalSeconds = 0.0;              // durée avec du contenu harmonique
    std::vector<Section> sections;
};

} // namespace aura::key
