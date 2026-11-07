# AuraPitch 1.0 — NextDSP · architecture (étapes 1 à 3)

Correction de hauteur et Pitch & Formant Shifter, VST3 / AU / Standalone, C++20 · JUCE 8.0.15 (même version
qu'AuraEngine, AuraDAW et AuraRack). Traitement en `double` de 44,1 kHz au DXD 352,8 / 384 kHz, entrée/sortie
`float` ou `double` selon l'hôte.

**État**
- Étape 1 : détecteur, gamme, courbe de correction, FIFO — banc `aurapitch_dsp_test` **247 / 247**.
- Étape 2 : PSOLA Live, vocodeur Studio/DXD, formants, moteur complet `AuraPitchEngine` — banc
  `aurapitch_engine_test` **136 / 136** (dont la compensation de niveau PSOLA en descente).
- Étape 3 : plug-in JUCE 8 (`Parameters`, `PluginProcessor`, éditeur NextDSP), CMake final, CI GitHub Actions —
  banc `aurapitch_plugin_test` **55 / 55**. Compilé et testé sous Linux avec JUCE 8.0.15 (VST3 + Standalone,
  zéro avertissement dans le code AuraPitch avec les avertissements recommandés de JUCE).
- Les bancs DSP passent avec GCC 13 et Clang 18 ; AddressSanitizer + UBSan et ThreadSanitizer ne signalent rien
  (seul le budget CPU échoue sous sanitizer, ce qui est attendu). Les en-têtes compilent sans aucun avertissement
  (`-Wall -Wextra -Wshadow -Wconversion -Wpedantic`).

## Arborescence

```
AuraPitch/
├── CMakeLists.txt                     JUCE 8.0.15 (FetchContent ou JUCE_DIR) ; noyau DSP sans JUCE ; cible AuraPitchTests
├── Source/
│   ├── DSP/                           en-têtes C++20 sans JUCE, testables seuls
│   │   ├── AuraPitchEngine.h          ✅ chef d'orchestre : détection -> gamme -> courbe -> Live / Studio -> Mix -> FIFO
│   │   ├── PitchDetector.h            ✅ décimation demi-bande + MPM + raffinement ×4 + voisement
│   │   ├── ScaleQuantizer.h           ✅ gammes, clavier Custom, A4 415–466, CorrectionCurve (Retune, Humanize…)
│   │   ├── PSOLAShifter.h             ✅ TD-PSOLA Live : marques PLL, grains Hann, formants temporels, fondu V/UV
│   │   ├── StudioPhaseVocoder.h       ✅ vocodeur Laroche-Dolson 2048 / 4096, découpe multicadence « delta »
│   │   ├── FormantProcessor.h         ✅ enveloppe vraie (cepstre itératif), déformation des formants
│   │   ├── FFT.h                      ✅ FFT réelle double (N/2 complexe + séparation)
│   │   ├── HalfBand.h                 ✅ demi-bandes ÷2 / ×2 génériques, anneau miroir
│   │   └── LockFreePitchFifo.h        ✅ SPSC statique audio -> interface (PitchFrame)
│   ├── Parameters.h/.cpp              ✅ APVTS (26 paramètres, identifiants figés), lecture temps réel, 4 presets d'usine
│   ├── PluginProcessor.h/.cpp         ✅ AuraPitchProcessor : 32/64 bits, mono/stéréo/double mono, bypass, latence
│   └── GUI/
│       ├── PluginEditor.h/.cpp        ✅ AuraPitchEditor (920 × 600, redimensionnable 900 × 560 … 1800 × 1150)
│       ├── NextDSPLookAndFeel.h/.cpp  ✅ couleurs, boutons rotatifs à anneau lumineux, listes, interrupteurs
│       ├── Controls.h/.cpp            ✅ Knob, SegmentedControl (paramètres Choice / Bool)
│       ├── PitchDisplay.h/.cpp        ✅ PitchGraph (historique défilant), TunerArc (accordeur ±50 c)
│       └── ScaleKeyboard.h/.cpp       ✅ clavier 12 notes Active / Bypass / Removed
├── Tests/
│   ├── PitchDspSelfTest.cpp           ✅ banc étape 1
│   ├── Step2SelfTest.cpp              ✅ banc étape 2
│   ├── PluginSelfTest.cpp             ✅ banc étape 3 (processeur + rendu de l'éditeur en PNG)
│   └── TestSupport.h                  signaux de test, mesures (F0, formants sur harmoniques, clics)
├── docs/screenshot.png                capture rendue par le banc (384 kHz, Studio DXD, 64 bits)
├── .github/workflows/build.yml        CI : macOS universel + auval, Windows x64, Linux, release sur v*
└── .gitignore
```


## Flux du signal

```
 entrée ─┬─► somme mono ─► PitchDetector ─(1,5 ms)─► ScaleQuantizer ─► CorrectionCurve ─► décalage (½ t)
         │                    │ période, voisé, frameCentre                                 │
         │                    ▼                                                             ▼
         │   Live   : PSOLAShifter ◄── dernière estimation, sans attente ───────────────────┤
         │   Studio : PitchControlTrack (horodatée frameCentre) ─► StudioDxdShifter ◄────────┘
         │                                                         (lit la commande au centre de chaque trame)
         └─► anneau d'entrée ─► Dry retardé de la latence du mode ─► Mix : Dry + Mix·(Wet − Dry) ─► sortie
                                PitchFrame ─► LockFreePitchFifo ─► interface (drain à 60 / 120 Hz)
```

## Latences (échantillons à la fréquence native)

| fs (kHz) | 44,1 | 48 | 88,2 | 96 | 176,4 | 192 | 352,8 | 384 |
|---|---|---|---|---|---|---|---|---|
| **Live** | 88 | 96 | 176 | 192 | 353 | 384 | 706 | 768 |
| | 2,0 ms | 2,0 ms | 2,0 ms | 2,0 ms | 2,0 ms | 2,0 ms | 2,0 ms | 2,0 ms |
| **Studio/DXD** | 5 057 | 5 137 | 10 260 | 10 420 | 20 667 | 20 987 | 41 481 | 42 121 |
| | 114,7 ms | 107,0 ms | 116,3 ms | 108,5 ms | 117,2 ms | 109,3 ms | 117,6 ms | 109,7 ms |

- **Live** : 2 ms fixes. La hauteur appliquée est la dernière estimation valide, dès sa sortie du détecteur.
  Contrainte causale acceptée : quand ratio ≠ 1, le contenu corrigé sort avec un retard de 2 ms à 2 ms + une période.
- **Studio** : retard du détecteur pour l'InputType le plus grave + un saut (≈ 22 ms) + vocodeur et découpe (≈ 85 ms).
  La valeur est constante : changer d'InputType ne change pas la latence annoncée. Le vocodeur lit la commande à
  l'instant du centre de chaque trame : sur une attaque, la sortie est juste dès la première milliseconde mesurable
  (0,0 ms au banc, contre 21 ms en Live).

## Alignement Dry / Wet et passages non voisés

Dans les deux modes, un passage non voisé (sifflante, plosive, souffle, silence) donne **Wet = Dry retardé,
échantillon pour échantillon**. Mix < 100 % ne crée donc ni peigne ni dédoublement sur les consonnes. Au banc, du
souffle traverse le moteur en Live comme en Studio, Mix 100 % ou 50 %, à 44,1 / 48 / 384 kHz, avec un écart à
l'entrée retardée de 0 à 1,7·10⁻¹⁶.

- **Live** : au rapport 1, les marques de synthèse sont verrouillées sur « marque d'analyse + 2 ms » et tous les
  grains lisent avec le même décalage : la sortie est exactement l'entrée retardée (écart 2,8·10⁻¹⁷, F0 vibrée).
  En non voisé, fondu d'une période vers ce retard pur. Pendant le bypass, les grains continuent d'être posés,
  verrouillés : la reprise part d'une sortie identique à l'entrée, sans clic.
- **Studio** : le vocodeur ne fournit qu'une correction Δ ajoutée à l'entrée retardée. Une trame non voisée est
  recopiée telle quelle (Δ = 0). Le fondu entre trames corrigées et neutres est celui de l'OLA (fenêtre de Hann de
  43 ou 85 ms), sans clic. Une trame est traitée comme voisée dès qu'une estimation voisée tombe dans le quart
  central de sa fenêtre : c'est ce qui cale la correction sur l'attaque.

## Classes

### `AuraPitchEngine` (✅)

| Méthode | Thread | Rôle |
|---|---|---|
| `prepare (fs, numChannels)` | prepareToPlay | prépare détecteur, PSOLA, Studio (les deux cœurs), anneaux, tampons |
| `setParameters (const AuraPitchParams&)` | audio, début de bloc | sans allocation |
| `process<T> (T* const* io, nc, n)` | audio | en place, `float` ou `double`, 1 ou 2 canaux, blocs de toute taille |
| `getLatencySamples ([mode])` | tous | à transmettre à `setLatencySamples()` |
| `getPitchFifo()` | interface | `drain()` des `PitchFrame` |

`AuraPitchParams` : `inputType`, `key`, `scale`, `customNotes[12]`, `retuneMs`, `humanize`, `formantShift`,
`formantPreserve`, `transpose`, `fineTuneCents`, `referenceA4`, `mode` (Live / Studio), `mix`. Mix est lissé sur
20 ms et FormantShift sur 30 ms. Un changement de mode fait un fondu de sortie de 5 ms, réinitialise le moteur
entrant, puis remonte en 5 ms. Le processeur JUCE annonce la nouvelle latence depuis le message thread.

### `PSOLAShifter` (✅, Live)

- **Marques d'analyse** : grille de période Ta (détecteur), verrouillée par une PLL sur le maximum du signal
  passe-bas (pic glottique) dans ±Ta/4 ; la grille est extrapolée vers l'avant.
- **Grains** : fenêtre de Hann de ±Ta dans le domaine d'entrée, posée sur des marques de synthèse espacées de
  Ta / ratio. Les grains ne copient rien : ils lisent l'anneau d'entrée (Hermite cubique) avec un décalage fixe.
- **OLA à gain constant** : normalisation par le recouvrement attendu (demi-fenêtre / espacement) ; en montée,
  chaque impulsion glottique garde son amplitude.
- **Formants dans le domaine temporel** : le contenu du grain est rééchantillonné d'un facteur α.
  - FormantPreserve : α = 2^(FormantShift/12).
  - Sinon : α = ratio × 2^(FormantShift/12).

  Au banc, la sortie reproduit à 0,0 dB près les amplitudes harmoniques de l'entrée rééchantillonnée.
- Jusqu'à 32 grains simultanés ; anneau de 0,35 s à fs ; aucune allocation après `prepare()`.

### `StudioDxdShifter` + `PhaseVocoderCore` (✅, Studio/DXD)

- **Découpe « delta »** :

  ```
  x ─► ÷2…÷2 ─► d (44,1/48 kHz) ─► vocodeur ─► v ─┐
                └──► retard ───────────────────────┴─(−)─► δ ─► ×2…×2 ─► Δ
  Wet = x retardé + Δ
  ```

  - Demi-bandes de 127 coefficients (≈ 110 dB). La latence de la chaîne est calibrée dans `prepare()` sur un
    signal à bande limitée. La voie vocale est alignée sur la voie directe à mieux que −130 dB de résidu, à
    toutes les fréquences d'échantillonnage.
  - L'air du DXD (au-delà de ~20 kHz) n'est ni filtré ni décalé : un 30 kHz superposé à une voix corrigée de
    +2 demi-tons ressort à +0,0003 dB et +0,001°.
- **Deux résolutions, même latence** :
  - FFT 2048 (43 ms) pour Soprano et Alto/Tenor : elle suit le vibrato. En hard-tune sur un vibrato de ±8 c, la
    sortie est à −0,06 c de la note, contre −1,4 c avec la FFT 4096.
  - FFT 4096 (85 ms) pour Low Male et Instrument : elle sépare les harmoniques jusqu'à 50 Hz. La FFT 2048 se
    trompe de 17 à 124 c sous 70 Hz.
  - Le cœur inactif reçoit l'entrée en permanence. Au changement de résolution, Δ s'éteint en 10 ms puis revient
    en 10 ms, sans changement de latence.
- **Vocodeur** (Laroche & Dolson 1999) : saut N/8, fréquence instantanée par différence de phase, régions
  d'influence des pics déplacées d'un nombre entier de bins et tournées d'un phaseur commun.
  - Verrouillage vertical : chaque région garde ses phases relatives.
  - Verrouillage horizontal : Z(u) = Z(u−1)·e^{jR(β−1)ω}.
  - Remise en phase sur les transitoires : flux spectral > 0,5 et énergie ×2 -> Z = 1. Sur des salves
    percussives, la corrélation de forme des 4 premières ms passe de −0,19 à 0,51 (FFT 2048) et de 0,04 à 0,33
    (FFT 4096) ; pré-écho de −25 dB et −22 dB.
- **Formants (True Formant Preservation)** : enveloppe vraie (cepstre itératif, ordre ½·fsV/F0), initialisée
  sur la log-magnitude interpolée entre les maxima harmoniques. Chaque bin déplacé reçoit le gain E'(k')/E(k), où E'
  est l'enveloppe déformée de γ. L'enveloppe ne dépend que de fsV (44,1 / 48 kHz) : elle est identique de 44,1 à
  384 kHz.
- **Stéréo** : pics, phases, enveloppe et rotations sont calculés sur la moyenne des canaux puis appliqués à chaque
  canal, ce qui conserve l'image et les phases inter-canaux.

### Étape 1 (inchangée, voir le banc 1)

`PitchDetector` (erreur max 0,033 c, 44,1–384 kHz, 0 % de sifflantes voisées), `ScaleQuantizer` (hystérésis
12 c, Custom Active / Bypass / Removed), `CorrectionCurve` (Retune log 0–400 ms, Humanize), `LockFreePitchFifo`.
Le décimateur du détecteur vient désormais de `HalfBand.h` (mêmes coefficients, résultats identiques).

## Paramètres (APVTS de l'étape 3, identifiants figés)

| ID | Plage | `AuraPitchParams` |
|---|---|---|
| `input_type` | Soprano, Alto/Tenor, Low Male, Instrument | `inputType` (règle aussi la FFT Studio) |
| `key` / `scale` | C … B / Chromatic, Major, Minor, Pentatonic, Custom | `key`, `scale` |
| `note_0` … `note_11` | Active, Bypass, Removed | `customNotes` |
| `retune_speed` | 0 … 400 ms (normalisé `401^x − 1`) | `retuneMs` |
| `humanize` | 0 … 100 % | `humanize` |
| `formant_shift` / `formant_preserve` | −12 … +12 dt / On-Off | `formantShift`, `formantPreserve` |
| `transpose` / `fine_tune` / `reference_a4` | ±24 dt / ±100 c / 415–466 Hz | `transpose`, `fineTuneCents`, `referenceA4` |
| `tracking_mode` | Live, Studio/DXD | `mode` (change la latence) |
| `mix` | 0 … 100 % | `mix` |

## Banc d'essai de l'étape 2 (`aurapitch_engine_test`, 136 vérifications)

| Mesure | Résultat |
|---|---|
| FFT réelle 64 … 32 768 points | écart à la DFT ≤ 8·10⁻¹², aller-retour ≤ 2,4·10⁻¹⁵ |
| Enveloppe vraie vs enveloppe exacte (F0 110 / 220 / 330 Hz, 44,1 / 48 / 384 kHz) | 0,40 à 1,53 dB RMS |
| PSOLA au rapport 1, stéréo, F0 vibrée, 6 fréquences de 44,1 à 384 kHz | sortie = entrée retardée de 2 ms (2,8·10⁻¹⁷) |
| PSOLA : justesse de ±1 à ±12 dt | moyenne ≤ 0,01 c, max 0,54 c (48 k) / 0,09 c (384 k) |
| Studio : justesse de ±1 à ±12 dt, 48 / 96 / 384 kHz | moyenne ≤ 0,01 c, max 0,03 c |
| Formants +5 dt, Preserve ON / OFF, FormantShift +4 dt (sur les harmoniques) | PSOLA +0,05 / +5,00 / +3,95 dt · Studio −0,15 / +4,75 / +4,15 dt |
| Transitions V/UV (énergie > 6 kHz, fenêtres de 0,5 ms) | PSOLA +0,8 dB sur le régime (bascule franche : +35 dB) · Studio +0,0 dB |
| Moteur, non voisé, Live / Studio, Mix 100 / 50 % | sortie = Dry retardé (≤ 1,7·10⁻¹⁶) |
| Hard-tune A3 +30 c ±8 c de vibrato, Retune 0 | Live −0,00 c (max 1,3) · Studio −0,06 c (max 0,8), 48 et 384 kHz |
| Attaque A4 +40 c, Retune 0 : sortie juste après | Live 21,1 ms · Studio 0,0 ms |
| Changement Live -> Studio à chaud, FIFO, E/S float vs double | sans NaN · 783 trames, hauteur exacte · écart 1,4·10⁻⁸ |
| Thread interface lisant la FIFO pendant le traitement à 384 kHz | 649 trames, dans l'ordre, 0 perdue, TSan muet |
| Allocations dans `process()` + `setParameters()` (automation à chaque bloc) | 0, à toutes les fréquences |

**Charge CPU**, stéréo, voix corrigée en continu (un cœur de Xeon 2,1 GHz virtuel, `-O2`) :

| fs (kHz) | 44,1 | 48 | 96 | 192 | 352,8 | 384 |
|---|---|---|---|---|---|---|
| Live | 1,2 % | 1,1 % | 1,3 % | 2,3 % | 2,8 % | 3,6 % |
| Studio/DXD | 4,2 % | 4,6 % | 5,1 % | 7,7 % | 9,7 % | 10,7 % |

## Limites et points ouverts

- **Signaux de test** : uniquement des voyelles, du bruit et des salves synthétiques. Un passage sur des voix
  réelles (fry, voix soufflées, sifflantes réelles, diphtongues) doit confirmer les seuils avant la mise en vente.
- **Non voisé = bypass complet** : Transpose et FormantShift ne s'appliquent pas aux consonnes non voisées
  (choix validé : rapport 1). Si un FormantShift global est voulu un jour, il faudra une option.
- **PSOLA en descente** : compensation analytique lissée 1/√max(r, ½) (jusqu'à +3 dB à −12 dt). Niveau RMS
  mesuré à −1 / −3 / −7 / −12 dt : −0,02 / −0,13 / −0,16 / −0,13 dB. En montée, +7 dt donne +1,7 dB sur la
  voyelle de test (pas de compensation demandée en montée).
- **Studio, InputType** : Alto/Tenor utilise la FFT 2048, juste jusqu'à ~82 Hz (0,4 c moyen). Pour une voix de basse,
  il faut choisir Low Male.

## Étape 3 — plug-in JUCE 8

### `AuraPitchProcessor`

- **Bus** : mono -> mono ou stéréo -> stéréo. En stéréo, `channel_mode` choisit **Linked** (un moteur, détection
  sur la somme mono, mêmes grains et mêmes rotations de phase sur les deux canaux : image conservée) ou **Dual Mono**
  (deux moteurs, un détecteur par canal). Le changement fait un fondu de sortie de 5 ms, réinitialise les moteurs
  entrants et remonte ; pendant la latence du mode, la sortie reste silencieuse.
- **Précision** : `supportsDoublePrecisionProcessing() = true`. `processBlock (AudioBuffer<float>&)` et
  `(AudioBuffer<double>&)` passent par un même `processTemplated<T>` : un seul `AuraPitchEngine`, calcul interne
  toujours en 64 bits, entrées / sorties en `float` ou `double` sans copie. Une classe moteur par type ne
  servirait à rien.
- **Bypass** : le paramètre `bypass` est déclaré à l'hôte (`getBypassParameter`). Il fait passer Mix à 0 en
  20 ms : la sortie devient le Dry retardé de la latence, donc la PDC reste juste et il n'y a ni clic ni saut de
  phase. `processBlockBypassed` (bypass propre à l'hôte) fait de même.
- **Latence** : annoncée dans `prepareToPlay` pour le mode courant et la fréquence préparée. Un timer du message
  thread (20 Hz) compare ensuite avec le TrackingMode demandé : `setLatencySamples()` +
  `updateHostDisplay (ChangeDetails().withLatencyChanged (true))`. L'appel ne vient jamais du thread audio. La
  latence Studio ne dépend pas de l'InputType.
- **Presets d'usine** (programmes de l'hôte + liste de l'éditeur) :

  | Preset | Mode | Réglages |
  |---|---|---|
  | Natural Vocal | Live | Retune 45 ms, Humanize 40 %, chromatique |
  | 0ms Hard-Tune Urban | Live | A mineur, Retune 0 ms, Humanize 0 % |
  | DXD Mastering Surgical | Studio DXD | Retune 25 ms, Humanize 30 % |
  | Deep Octave Formant | Live | Transpose −12, Formant −3 dt (Preserve), Mix 50 % |

  Chaque paramètre est notifié à l'hôte (undo / automation).
- **État** : XML de l'APVTS + version + programme courant ; restauration vérifiée sur les 26 paramètres.

### Éditeur

- Fond `#0B0C10`, panneaux `#12151E`, bordures `#1E2433`, cyan `#00F0FF` (corrigé / cible), violet `#A855F7` ->
  ambre `#F59E0B` (voix entrante, de l'ancien au récent).
- **En-tête** : logo, presets d'usine, `LIVE 2 MS | STUDIO DXD`, `BYPASS`, puis les badges : fréquence (`384.0 kHz`),
  `DXD` lumineux à partir de 352,8 kHz, `32-BIT` / `64-BIT` selon la précision de l'hôte, `LAT x ms` (violet en
  Studio).
- **Graphe** : historique de 4 s sur une grille de demi-tons (notes de la gamme éclairées, note cible surlignée),
  cadrage vertical automatique, défilement calé sur l'horloge audio, au plus un point par pixel. Il est alimenté à
  60 images/s par un `juce::Timer` qui vide la `LockFreePitchFifo`.
- **Accordeur** : arc de −50 à +50 cents, zone juste ±5 c. L'aiguille ambre montre la voix entrante (cyan quand
  elle est juste), le repère cyan la sortie corrigée. La note cible est affichée en grand, avec les cents, les Hz
  d'entrée et l'écart de sortie.
- **Clavier 12 notes** : clic = Active <-> Removed, Alt-clic ou clic droit = Bypass. Sur une gamme prédéfinie, le
  premier clic recopie la gamme dans le clavier et passe en Custom. Le clavier est synchronisé avec Key / Scale et
  les automations.
- **Boutons rotatifs** : Retune, Humanize, Formant, Transpose, Fine Tune, A4 Ref, Mix. Valeurs avec unité,
  saisissables au clavier, double-clic pour revenir à la valeur par défaut. Interrupteur Preserve ; sélecteurs
  segmentés Input Type et Stereo.

### Banc `aurapitch_plugin_test` (55 vérifications)

| Mesure | Résultat |
|---|---|
| Paramètres, textes avec unités, courbe Retune | 26 paramètres · « 0.0 ms » / « 19 ms » / « 400 ms », « +3.00 st », « 440.0 Hz » |
| Presets | 4, appliqués et lus par l'hôte |
| Latence annoncée, 44,1 / 96 / 352,8 / 384 kHz | Live 88 / 192 / 706 / 768 -> Studio 5 057 / 10 420 / 41 481 / 42 121 -> Live ; inchangée avec Low Male |
| Hard-tune A3 +35 c, 32 et 64 bits, Live et Studio | +0,00 / +0,01 c |
| Double mono (G3 −30 c à gauche, D4 +25 c à droite) | −0,02 c / +0,03 c |
| Bypass en cours de lecture (Live, Studio) | sortie = entrée retardée de la latence (7·10⁻⁹, précision float) ; pente au fondu égale à celle du signal |
| `processBlockBypassed` 64 bits | Dry retardé exact |
| État sauvegardé -> nouvelle instance | identique (1 004 octets) |
| Bus mono, blocs de 333 échantillons | corrigé |
| Éditeur 920 × 600, 1400 × 860, 900 × 560 | rendus PNG (voir `docs/screenshot.png`) |

### Limites de l'étape 3

- **Plateformes** : compilé et testé sous Linux uniquement (VST3, Standalone, bancs). macOS et Windows passent pour
  la première fois par la CI : le premier push dira si Apple Clang et MSVC acceptent le code tel quel. AU et `auval`
  ne sont essayés que sur le runner macOS (étape non bloquante tant qu'elle n'a pas été vue au vert).
- **Hôtes réels** : aucune séance dans un DAW (Logic, Cubase, Ableton, Reaper, Pro Tools via AAX non prévu).
  pluginval n'a pas été lancé.
- **Polices** : celles du système (pas de police embarquée) ; le rendu varie légèrement d'un OS à l'autre.
- **Changement Linked <-> Dual Mono** : silence de la durée de la latence du mode pendant la reprise (≈ 110 ms en
  Studio).
