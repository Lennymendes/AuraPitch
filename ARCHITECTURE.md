# AuraSuite — AuraPitch 1.0 + AuraKey 1.0 — NextDSP · architecture (étapes 1 à 6)

AuraPitch : correction de hauteur et Pitch & Formant Shifter. AuraKey (étape 6) : analyseur de tonalité, tempo et
sections. **VST3 uniquement** depuis l'étape 6 (plus d'Audio Unit ni de Standalone). C++20 · JUCE 8.0.15 (même version
qu'AuraEngine, AuraDAW et AuraRack). Traitement en `double` de 44,1 kHz au DXD 352,8 / 384 kHz, entrée/sortie
`float` ou `double` selon l'hôte.

**État**
- Étape 1 : détecteur, gamme, courbe de correction, FIFO — banc `aurapitch_dsp_test` **247 / 247**.
- Étape 2 : PSOLA Live, vocodeur Studio/DXD, formants, moteur complet `AuraPitchEngine` — banc
  `aurapitch_engine_test` **136 / 136** (dont la compensation de niveau PSOLA en descente).
- Étape 3 : plug-in JUCE 8 (`Parameters`, `PluginProcessor`, éditeur NextDSP), CMake final, CI GitHub Actions —
  banc `aurapitch_plugin_test` **55 / 55**. Compilé et testé sous Linux avec JUCE 8.0.15 (VST3 + Standalone,
  zéro avertissement dans le code AuraPitch avec les avertissements recommandés de JUCE).
- Étape 4 : refonte du cœur après l'écoute sur acapella réelle (dédoublement, voile 2–12 kHz, timbre métallique) :
  `SpliceShifter` (une seule tête de lecture, raccords glottiques), interpolation sinc 48 points, filtre de formants
  différentiel en Studio, voisement maintenu 50 ms. Banc `aurapitch_realvoice_test` **32 / 32** ; banc de l'étape 2
  adapté : **134 / 134**. Latence Studio ramenée de ≈ 110 ms à ≈ 56–62 ms. CI au vert sur macOS (auval),
  Windows x64 et Linux, les quatre bancs, aucun avertissement du compilateur dans le code AuraPitch.
- Étape 5A : moteur expressif (Flex / Transition, Throat, Doubler, Auto-Key, MIDI Target) et ergonomie de l'éditeur
  (glissé fin, saisie au clavier, FREEZE, AUTO-KEY, MIDI). Banc `aurapitch_step5a_test` **209 / 209**, banc du
  plug-in **73 / 73**. Réglages neutres (Throat 100 %, Doubler 0 %, Transition 0, MIDI Off) : sortie **identique au
  bit près** à celle de l'étape 4 (vérifié contre le commit `0afb1e8`, 6 fréquences, Live et Studio). Les cinq
  bancs passent sous Linux (GCC 13, JUCE 8.0.15, sans avertissement) ; la CI GitHub n'a pas encore tourné pour cette
  étape : GitHub refuse de démarrer les jobs tant que la facturation du compte n'est pas réglée.
- Étape 5B : GRAPH EDITOR calé sur la timeline du DAW, sans audio en RAM (`NoteGraphModel.h`, `GraphEditor`). Banc
  `aurapitch_step5b_test` **66 / 66** (48 sans JUCE ; ASan + UBSan + LeakSanitizer et ThreadSanitizer muets). Sans
  notes, ou transport arrêté : sortie identique au bit près à celle de l'étape 4. Validation locale (Linux, GCC 13,
  JUCE 8.0.15, sans avertissement) ; la CI GitHub reste arrêtée par le quota de minutes du compte.
- Étape 6 : VST3 seul (CMake et CI) ; GRAPH EDITOR en fenêtre agrandie (`GraphWindow`) ; nouveau plug-in **AuraKey**.
  Banc `aurapitch_step5b_test` **83 / 83** (17 vérifications de la vue agrandie) ; banc `aurakey_test` **159 / 159**
  avec JUCE, 72 / 72 sans JUCE. Les sept bancs passent (`ctest` sous Xvfb, GCC 13, JUCE 8.0.15, aucun avertissement
  dans le code de la suite) ; ASan + UBSan et TSan muets sur les noyaux DSP. CI GitHub arrêtée par le quota macOS.
- Les bancs DSP passent avec GCC 13 et Clang 18 ; AddressSanitizer + UBSan et ThreadSanitizer ne signalent rien
  (seul le budget CPU échoue sous sanitizer, ce qui est attendu). Les en-têtes compilent sans aucun avertissement
  (`-Wall -Wextra -Wshadow -Wconversion -Wpedantic`).

## Arborescence

```
AuraPitch/                             (dépôt AuraSuite)
├── CMakeLists.txt                     JUCE 8.0.15 (FetchContent ou JUCE_DIR) ; AuraPitch VST3 ; include d'AuraKey ; AuraPitchTests
├── Source/
│   ├── DSP/                           en-têtes C++20 sans JUCE, testables seuls
│   │   ├── AuraPitchEngine.h          ✅ chef d'orchestre : détection -> gamme -> courbe -> Live / Studio -> Mix -> FIFO
│   │   ├── PitchDetector.h            ✅ décimation demi-bande + MPM + raffinement ×4 + voisement
│   │   ├── ScaleQuantizer.h           ✅ gammes, clavier Custom, A4 415–466, CorrectionCurve (Retune, Humanize…)
│   │   ├── SpliceShifter.h            ✅ étape 4 : tête de lecture unique, raccords synchrones de la période (T* sous-échantillon)
│   │   ├── Interpolator.h             ✅ étape 4 : sinc fenêtré Kaiser 48 points, 512 phases, 29 bancs anti-repliement
│   │   ├── PSOLAShifter.h             ✅ TD-PSOLA (effets) : marques par corrélation locale, grains sinc, fondu V/UV
│   │   ├── StudioFormantFilter.h      ✅ étape 4 : Studio/DXD = découpe « delta » + raccords + filtre de formants différentiel
│   │   ├── FormantProcessor.h         ✅ enveloppe vraie (cepstre itératif) ; ThroatModel (étape 5A)
│   │   ├── AutoKeyDetector.h          ✅ étape 5A : histogramme chromatique -> tonalité + confiance
│   │   ├── NoteGraphModel.h           ✅ étape 5B : capture, segmentation en notes, snapshot sans verrou, sérialisation
│   │   ├── FFT.h                      ✅ FFT réelle double (N/2 complexe + séparation)
│   │   ├── HalfBand.h                 ✅ demi-bandes ÷2 / ×2 génériques, anneau miroir
│   │   └── LockFreePitchFifo.h        ✅ SPSC statique audio -> interface (PitchFrame)
│   ├── Parameters.h/.cpp              ✅ APVTS (30 paramètres, identifiants figés), lecture temps réel, 4 presets d'usine
│   ├── PluginProcessor.h/.cpp         ✅ AuraPitchProcessor : 32/64 bits, mono/stéréo/double mono, bypass, latence
│   └── GUI/
│       ├── PluginEditor.h/.cpp        ✅ AuraPitchEditor (920 × 600, redimensionnable 900 × 560 … 1800 × 1150)
│       ├── NextDSPLookAndFeel.h/.cpp  ✅ couleurs, boutons rotatifs à anneau lumineux, listes, interrupteurs
│       ├── Controls.h/.cpp            ✅ Knob, SegmentedControl (paramètres Choice / Bool)
│       ├── PitchDisplay.h/.cpp        ✅ PitchGraph (historique défilant), TunerArc (accordeur ±50 c)
│       ├── GraphEditor.h/.cpp         ✅ étape 5B : GRAPH EDITOR (piano-roll calé sur la timeline, édition à la souris)
│       ├── GraphWindow.h/.cpp         ✅ étape 6 : fenêtre agrandie du GRAPH EDITOR + panneau de renvoi
│       └── ScaleKeyboard.h/.cpp       ✅ clavier 12 notes Active / Bypass / Removed
├── Tests/
│   ├── PitchDspSelfTest.cpp           ✅ banc étape 1
│   ├── Step2SelfTest.cpp              ✅ banc étape 2
│   ├── PluginSelfTest.cpp             ✅ banc étape 3 (processeur + rendu de l'éditeur en PNG)
│   ├── RealVoiceSelfTest.cpp          ✅ banc étape 4 (voix réaliste : jitter, shimmer, souffle, consonnes, flow rapide)
│   ├── Step5ASelfTest.cpp             ✅ banc étape 5A (Auto-Key, MIDI, Flex, Throat, Doubler, transparence, CPU, temps réel)
│   ├── Step5BTests.cpp                ✅ banc étape 5B (GRAPH EDITOR ; moteur seul, ou + processeur et interface avec JUCE)
│   ├── VocalRealism.h                 générateur de voix réaliste, rejet de voix fantôme, écarts par tiers d'octave
│   └── TestSupport.h                  signaux de test, mesures (F0, formants sur harmoniques, clics)
├── docs/screenshot.png                capture rendue par le banc (384 kHz, Studio DXD, 64 bits)
├── docs/screenshot-5a.png             MIDI Target (notes tenues éclairées), AUTO-KEY appliqué, Throat / Doubler / Flex
├── docs/screenshot-graph.png          GRAPH EDITOR : phrase capturée, note éditée (D4, vibrato 0), glissé, scission
├── docs/screenshot-graph-window.png   étape 6 : GRAPH EDITOR dans sa fenêtre 1200 × 800
├── docs/screenshot-aurakey.png        étape 6 : AuraKey (clé live, cercle des quintes, fichier analysé, sections)
├── AuraKey/
│   ├── AuraKey.cmake                  cible VST3 AuraKey, noyau aurakey_dsp, banc aurakey_test (inclus par la racine)
│   ├── Source/
│   │   ├── DSP/                       en-têtes C++20 sans JUCE
│   │   │   ├── Decimator.h            RIF sinc-Blackman, flux -> ≈ 11 kHz (÷4 à 44,1/48 k … ÷35 à 384 k)
│   │   │   ├── ChromaAnalyzer.h       pics spectraux (FFT 8192) -> chroma 36 cases + accord (moyenne circulaire)
│   │   │   ├── LiveKeyAnalyzer.h      thread audio : chroma -> AutoKeyDetector (5A), mémoire, seqlock vers l'interface
│   │   │   ├── TempoEstimator.h       flux spectral, autocorrélation, peigne 4 temps + a priori 120 BPM
│   │   │   ├── OfflineKeyAnalyzer.h   fichier complet : clé, tempo, accord, sections (Viterbi)
│   │   │   └── KeyTypes.h             noms, Camelot, cercle des quintes, résultats
│   │   ├── PluginProcessor.h/.cpp     AuraKeyProcessor : passe-tout au bit près, analyse live, fil d'import, état
│   │   └── GUI/                       AuraKeyEditor (+ FilePanelView), CircleOfFifths, SectionMap
│   └── Tests/                         AuraKeyTests.cpp, PluginTests.inc (avec JUCE), SyntheticMusic.h
├── .github/workflows/build.yml        CI : macOS universel, Windows x64, Linux (VST3 des deux plug-ins), release sur v*
└── .gitignore
```


## Flux du signal

```
 entrée ─┬─► somme mono ─► PitchDetector ─(1,5 ms)─► ScaleQuantizer ─► CorrectionCurve ─► décalage (½ t)
         │                    │ période, voisé, frameCentre                                 │
         │                    ▼                                                             ▼
         │   Live   : SpliceShifter | PSOLAShifter (fondu 10 ms) ◄── dernière estimation ───────┤
         │   Studio : PitchControlTrack (horodatée frameCentre) ─► StudioDxdShifter ◄────────┘
         │                                         (lit la commande tous les 32 échantillons vocaux)
         └─► anneau d'entrée ─► Dry retardé de la latence du mode ─► Mix : Dry + Mix·(Wet − Dry) ─► sortie
                                PitchFrame ─► LockFreePitchFifo ─► interface (drain à 60 / 120 Hz)
```

## Latences (échantillons à la fréquence native)

| fs (kHz) | 44,1 | 48 | 88,2 | 96 | 176,4 | 192 | 352,8 | 384 |
|---|---|---|---|---|---|---|---|---|
| **Live** | 88 | 96 | 176 | 192 | 353 | 384 | 706 | 768 |
| | 2,0 ms | 2,0 ms | 2,0 ms | 2,0 ms | 2,0 ms | 2,0 ms | 2,0 ms | 2,0 ms |
| **Studio/DXD** | 2 585 | 2 673 | 5 316 | 5 492 | 10 779 | 11 131 | 21 705 | 22 409 |
| | 58,6 ms | 55,7 ms | 60,3 ms | 57,2 ms | 61,1 ms | 58,0 ms | 61,5 ms | 58,4 ms |

- **Live** : 2 ms fixes. La hauteur appliquée est la dernière estimation valide, dès sa sortie du détecteur.
  Contrainte causale acceptée : quand ratio ≠ 1, le contenu corrigé sort avec un retard de 2 ms à 2 ms + une période.
- **Studio** : retard du détecteur pour l'InputType le plus grave + un saut (≈ 22 ms) + découpe multicadence,
  raccords (2 ms) et filtre de formants (fenêtre 1024 à 44,1/48 kHz, ≈ 30 ms). La valeur est constante : changer
  d'InputType ne change pas la latence annoncée. La commande est lue à l'instant exact du signal traité : sur une
  attaque, la sortie est juste dès la première milliseconde mesurable.

## Alignement Dry / Wet et passages non voisés

Dans les deux modes, un passage non voisé (sifflante, plosive, souffle, silence) donne **Wet = Dry retardé,
échantillon pour échantillon**. Mix < 100 % ne crée donc ni peigne ni dédoublement sur les consonnes. Au banc, du
souffle traverse le moteur en Live comme en Studio, Mix 100 % ou 50 %, à 44,1 / 48 / 384 kHz, avec un écart à
l'entrée retardée de 0 à 1,7·10⁻¹⁶.

- **Live** : au rapport 1, les marques de synthèse sont verrouillées sur « marque d'analyse + 2 ms » et tous les
  grains lisent avec le même décalage : la sortie est exactement l'entrée retardée (écart 2,8·10⁻¹⁷, F0 vibrée).
  En non voisé, fondu d'une période vers ce retard pur. Pendant le bypass, les grains continuent d'être posés,
  verrouillés : la reprise part d'une sortie identique à l'entrée, sans clic.
- **Studio** : la voie vocale ne fournit qu'une correction Δ ajoutée à l'entrée retardée. En non voisé, le cœur
  de raccords revient au retard nominal par un fondu de 10 ms puis s'y verrouille, et le filtre de formants passe
  à gain 1 : Δ = 0 exactement.

## Classes

### `AuraPitchEngine` (✅)

| Méthode | Thread | Rôle |
|---|---|---|
| `prepare (fs, numChannels)` | prepareToPlay | prépare détecteur, raccords, PSOLA, Studio, anneaux, tampons |
| `setParameters (const AuraPitchParams&)` | audio, début de bloc | sans allocation |
| `process<T> (T* const* io, nc, n)` | audio | en place, `float` ou `double`, 1 ou 2 canaux, blocs de toute taille |
| `getLatencySamples ([mode])` | tous | à transmettre à `setLatencySamples()` |
| `getPitchFifo()` | interface | `drain()` des `PitchFrame` |

`AuraPitchParams` : `inputType`, `key`, `scale`, `customNotes[12]`, `retuneMs`, `humanize`, `formantShift`,
`formantPreserve`, `transpose`, `fineTuneCents`, `referenceA4`, `mode` (Live / Studio), `mix`, et depuis l'étape 5A
`transitionMs`, `throat`, `doubler`, `midiTarget`. Méthodes 5A : `addMidiEvent()` (thread audio, avant `process()`),
`getHeldMidiNotes()`, `startKeyDetection()` / `stopKeyDetection()` / `getKeyDetection()` (interface). Mix est lissé sur
20 ms et FormantShift sur 30 ms. Un changement de mode fait un fondu de sortie de 5 ms, réinitialise le moteur
entrant, puis remonte en 5 ms. Le processeur JUCE annonce la nouvelle latence depuis le message thread.

Choix du cœur (`usePsola()`) : PSOLA si |FormantShift| > 0,01 dt, ou si Preserve est actif avec |Transpose| ≥ 2 dt ;
sinon `SpliceShifter`. En Live, le passage de l'un à l'autre est un fondu de 10 ms. Le voisement est maintenu
50 ms après la dernière trame voisée (même note, même période) : une micro-coupure du détecteur au milieu d'une
voyelle ne fait plus basculer la sortie vers le Dry puis revenir.

### `SpliceShifter` (✅, étape 4 : correction courante, Live et Studio)

- **Une seule voix** : une tête de lecture parcourt l'anneau d'entrée à la vitesse r = ratio. Le retard dérive de
  (1 − r) par échantillon ; quand il sort de [oMin, oMin + 1,25 T], la tête saute d'une période vers l'avant ou
  l'arrière. Entre deux raccords, la sortie est l'entrée rééchantillonnée : aucune superposition de grains, donc ni
  peigne ni dédoublement.
- **Raccord glottique** : la longueur du saut T* est cherchée par corrélation normalisée dans ±10 % de la période
  du détecteur (passe grossière puis fine), avec raffinement parabolique sous-échantillon. Avec une période entière,
  les raccords se répétaient à F_in − F_out et laissaient une raie à F_in à −60 dB ; en sous-échantillon, 83 à 99 dB.
- **Fondu de raccord** : longueur min(T, 0,8 T / |1 − r|), normalisé par 1/√(g0² + g1² + 2ρ·g0·g1) (ρ = corrélation
  mesurée) : niveau constant, que les deux lectures soient en phase ou non.
- **Lecture** : `SincInterpolator`, banc anti-repliement choisi selon r. En stéréo, mêmes positions de lecture et
  mêmes raccords sur les deux canaux (recherche sur la somme mono).
- **Non voisé / rapport 1** : fondu de 10 ms vers le retard nominal D puis verrouillage : Wet = Dry exactement.

### `SincInterpolator` (✅, étape 4)

Sinc fenêtré Kaiser, 48 points, 512 phases interpolées linéairement entre elles, 29 bancs de coupure
(pas ×1,06) pour lire plus vite que 1 sans repliement. Gain DC normalisé à 1, exact aux positions entières.
Erreur mesurée ≤ −85 dB jusqu'à 20 kHz à 48 kHz ; l'Hermite cubique qu'il remplace perdait −0,5 dB à 12 kHz,
−1,4 dB à 16 kHz et −2,5 dB à 20 kHz (c'était une part du voile).

### `PSOLAShifter` (✅, effets de formants et grandes transpositions)

- **Marques d'analyse** : chaque marque est propagée de la précédente par corrélation croisée locale sur le signal
  passe-bas (±Ta/4, raffinement parabolique, seuil 0,5), ancrée sur le centre d'énergie de la période (énergie
  lissée 0,25 ms) : les marques restent sur le même point du cycle glottique, et les grains successifs sont en
  phase. Ancrées sur le maximum passe-bas, elles donnaient des creux de −8 à −11 dB sur une entrée décimée.
- **Grains** : fenêtre de Hann de ±Ta dans le domaine d'entrée, posée sur des marques de synthèse espacées de
  Ta / ratio. Les grains ne copient rien : ils lisent l'anneau d'entrée (sinc 48 points, banc selon α) avec un
  décalage fixe. `setActive (false)` arrête le rendu quand le moteur utilise les raccords.
- **OLA à gain constant** : normalisation par le recouvrement attendu (demi-fenêtre / espacement) ; en montée,
  chaque impulsion glottique garde son amplitude.
- **Formants dans le domaine temporel** : le contenu du grain est rééchantillonné d'un facteur α.
  - FormantPreserve : α = 2^(FormantShift/12).
  - Sinon : α = ratio × 2^(FormantShift/12).

  Au banc, la sortie reproduit à 0,0 dB près les amplitudes harmoniques de l'entrée rééchantillonnée.
- Jusqu'à 32 grains simultanés ; anneau de 0,35 s à fs ; aucune allocation après `prepare()`.

### `StudioDxdShifter` + `FormantFilterCore` (✅, Studio/DXD, refait à l'étape 4)

Le vocodeur de phase de l'étape 2 (`StudioPhaseVocoder.h`, supprimé) étalait les attaques (fenêtres 43 / 85 ms) et
donnait le timbre « phasé » entendu sur les inflexions rapides. Il est remplacé par :

- **Découpe « delta »** (inchangée) :

  ```
  x ─► ÷2…÷2 ─► d (44,1/48 kHz) ─► raccords | PSOLA ─► filtre de formants ─► v ─┐
                └──► retard ───────────────────────┴─(−)─► δ ─► ×2…×2 ─► Δ
  Wet = x retardé + Δ
  ```

  - Demi-bandes de 127 coefficients (≈ 110 dB). La latence de la chaîne est calibrée dans `prepare()` sur un
    signal à bande limitée. La voie vocale est alignée sur la voie directe à mieux que −130 dB de résidu, à
    toutes les fréquences d'échantillonnage.
  - L'air du DXD (au-delà de ~20 kHz) n'est ni filtré ni décalé : un 30 kHz superposé à une voix corrigée de
    +2 demi-tons ressort à +0,0003 dB et +0,001°.
- **Transposition** : `SpliceShifter` (ou PSOLA selon `usePsola()`) à fsV. Toute la bande vocale passe par la voie
  corrigée : il n'y a pas de seconde voix non corrigée laissée au-dessus d'une coupure.
- **Filtre de formants différentiel** : la sortie transposée est filtrée par G(k) = exp(E(k/γ) − E(k)), où E est
  l'enveloppe vraie (cepstre itératif) du signal transposé et γ = 2^(FormantShift/12) / r avec Preserve
  (2^(FormantShift/12) sinon). Le filtre ne touche qu'au module : la phase, et donc les attaques et la forme
  d'onde glottique, restent celles des raccords. Fenêtre de Hann 1024, FFT 2048 avec bourrage, saut 256 ;
  latence n − 1 + m/4. En non voisé ou avec PSOLA (qui déforme déjà les formants), G = 1.
- **Commande** : relue tous les 32 échantillons vocaux à l'instant du signal traité (retard des raccords pris en
  compte).
- **Stéréo** : raccords, gains et filtre calculés sur la somme mono et appliqués à chaque canal : mêmes marques,
  même phase.

### Étape 1 (inchangée, voir le banc 1)

`PitchDetector` (erreur max 0,033 c, 44,1–384 kHz, 0 % de sifflantes voisées), `ScaleQuantizer` (hystérésis
12 c, Custom Active / Bypass / Removed), `CorrectionCurve` (Retune log 0–400 ms, Humanize), `LockFreePitchFifo`.
Le décimateur du détecteur vient désormais de `HalfBand.h` (mêmes coefficients, résultats identiques).

## Paramètres (APVTS de l'étape 3, identifiants figés)

| ID | Plage | `AuraPitchParams` |
|---|---|---|
| `input_type` | Soprano, Alto/Tenor, Low Male, Instrument | `inputType` (plage du détecteur) |
| `key` / `scale` | C … B / Chromatic, Major, Minor, Pentatonic, Custom | `key`, `scale` |
| `note_0` … `note_11` | Active, Bypass, Removed | `customNotes` |
| `retune_speed` | 0 … 400 ms (normalisé `401^x − 1`) | `retuneMs` |
| `humanize` | 0 … 100 % | `humanize` |
| `formant_shift` / `formant_preserve` | −12 … +12 dt / On-Off | `formantShift`, `formantPreserve` |
| `transpose` / `fine_tune` / `reference_a4` | ±24 dt / ±100 c / 415–466 Hz | `transpose`, `fineTuneCents`, `referenceA4` |
| `tracking_mode` | Live, Studio/DXD | `mode` (change la latence) |
| `mix` | 0 … 100 % | `mix` |
| `transition` (5A) | 0 … 200 ms | `transitionMs` |
| `throat` (5A) | 80 … 120 % | `throat` |
| `doubler` (5A) | 0 … 100 % | `doubler` |
| `midi_target` (5A) | On / Off | `midiTarget` |

## Banc d'essai de l'étape 2 (`aurapitch_engine_test`, 134 vérifications après l'étape 4)

Les tests du vocodeur (FFT 2048 / 4096, remise en phase) ont été remplacés par ceux des raccords et du filtre
différentiel. Valeurs mises à jour à l'étape 4 : formants Studio +1,5 dt par raccords + filtre, écart +0,00 dt
(0,59 dB) ; attaque Studio comparée à la salve idéalement rééchantillonnée, corrélation 0,985, pré-écho −99 dB ;
transitions V/UV Studio +1,8 dB.


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
| Live | 1,8 % | 2,2 % | 4,7 % | 5,3 % | 9,6 % | 11,3 % |
| Studio/DXD | 2,4 % | 3,0 % | 3,2 % | 6,1 % | 6,9 % | 6,2 % |

Étape 4 : seul le cœur qui s'entend est rendu (l'autre avance son état pour un fondu propre), et le noyau sinc est
calculé une fois par lecture pour tous les canaux (lecture contiguë, vectorisée). Sans cela, la première version
de l'étape 4 montait à 18,5 % en local et 36,7 % sur le runner Linux de la CI à 384 kHz. Les runners de la CI sont
environ deux fois plus lents que cette machine.

## Limites et points ouverts

- **Signaux de test** : depuis l'étape 4, voix synthétique réaliste (jitter, shimmer, souffle, consonnes, flow
  rapide, vibrato). Une nouvelle écoute sur acapella réelle reste la validation finale.
- **Non voisé = bypass complet** : Transpose et FormantShift ne s'appliquent pas aux consonnes non voisées
  (choix validé : rapport 1). Si un FormantShift global est voulu un jour, il faudra une option.
- **PSOLA en descente** : compensation analytique lissée 1/√max(r, ½) (jusqu'à +3 dB à −12 dt). Niveau RMS
  mesuré à −1 / −3 / −7 / −12 dt : −0,02 / −0,13 / −0,16 / −0,13 dB. En montée, +7 dt donne +1,7 dB sur la
  voyelle de test (pas de compensation demandée en montée).
- **Studio, InputType** : ne règle plus que la plage du détecteur (le vocodeur à deux résolutions a disparu).

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

### Banc `aurapitch_plugin_test` (73 vérifications depuis l'étape 5A)

| Mesure | Résultat |
|---|---|
| Paramètres, textes avec unités, courbe Retune | 30 paramètres (26 + 4 de l'étape 5A) · « 0.0 ms » / « 19 ms » / « 400 ms », « +3.00 st », « 440.0 Hz » |
| Presets | 4, appliqués et lus par l'hôte |
| Latence annoncée, 44,1 / 96 / 352,8 / 384 kHz | Live 88 / 192 / 706 / 768 -> Studio 2 585 / 5 492 / 21 705 / 22 409 -> Live ; inchangée avec Low Male |
| Hard-tune A3 +35 c, 32 et 64 bits, Live et Studio | +0,00 / +0,01 c |
| Double mono (G3 −30 c à gauche, D4 +25 c à droite) | −0,02 c / +0,03 c |
| Bypass en cours de lecture (Live, Studio) | sortie = entrée retardée de la latence (7·10⁻⁹, précision float) ; pente au fondu égale à celle du signal |
| `processBlockBypassed` 64 bits | Dry retardé exact |
| État sauvegardé -> nouvelle instance | identique (1 004 octets) |
| Bus mono, blocs de 333 échantillons | corrigé |
| Éditeur 920 × 600, 1400 × 860, 900 × 560 | rendus PNG (voir `docs/screenshot.png`) |

### Limites de l'étape 3

- **Plateformes** : CI GitHub Actions au vert sur macOS (Apple Clang, binaire universel x86_64 + arm64 vérifié par
  `lipo`, **AU VALIDATION SUCCEEDED** avec `auval`), Windows x64 (MSVC, Visual Studio 18) et Linux : les trois bancs
  passent partout, et aucun avertissement du compilateur ne vient du code AuraPitch. Le premier passage Windows a
  révélé un débordement de pile (pile de 1 Mo) dans le banc : la FIFO et la piste de commande du moteur sont
  désormais allouées sur le tas à la construction (`sizeof (AuraPitchEngine)` : 210 ko -> 45 ko).
- **Hôtes réels** : aucune séance dans un DAW (Logic, Cubase, Ableton, Reaper, Pro Tools via AAX non prévu).
  pluginval n'a pas été lancé.
- **Polices** : celles du système (pas de police embarquée) ; le rendu varie légèrement d'un OS à l'autre.
- **Changement Linked <-> Dual Mono** : silence de la durée de la latence du mode pendant la reprise (≈ 60 ms en
  Studio).

## Étape 4 — voix réelle : dédoublement, voile, timbre métallique

### Diagnostic (mesuré sur l'ancien moteur)

| Défaut entendu | Cause | Correction |
|---|---|---|
| « 2 voix », chorus | PSOLA : grains superposés lus à des décalages différents (peigne) ; vocodeur Studio : phasiness ; basculements V/UV au milieu des voyelles | `SpliceShifter` (une seule lecture, T* sous-échantillon), voisement maintenu 50 ms |
| Voile 2–12 kHz | Hermite cubique (−0,5 dB à 12 kHz, −2,5 dB à 20 kHz) et peigne PSOLA : −1,4 à −3,4 dB au-dessus de 4 kHz | sinc 48 points, une seule lecture |
| Métallique sur inflexions rapides | marques PSOLA non glottiques ; fenêtres de 43 / 85 ms du vocodeur qui étalent les attaques | marques par corrélation locale, filtre de formants à module seul (phase intacte) |

À 48 kHz, il n'y a pas de découpe en fréquence (fsV = fs) : toute la bande est déjà traitée par la voie corrigée, et
la découpe des fréquences plus hautes s'annule à −130 dB. Le crossover à ~18 kHz proposé n'avait donc pas de
cause à corriger à 48 kHz ; aux fréquences DXD, la découpe existante est conservée (au-dessus de 20–24 kHz).

### Banc `aurapitch_realvoice_test` (32 vérifications)

| Mesure | Résultat |
|---|---|
| Interpolation, 48 kHz, positions aléatoires | sinc ≤ −85 dB jusqu'à 20 kHz (Hermite : −7 dB d'erreur à 20 kHz) |
| Voix fantôme, voix stable A3 +40 c -> A3 | Live 87,4 dB · Studio 83,2 dB (objectif > 60 dB) |
| Voix fantôme, voix réelle (jitter 0,1 %, shimmer 0,3 dB, souffle −30 dB) | Live 50,8 · Studio 52,7 dB, la voix idéale chantée juste n'atteint que 44,7 dB : le jitter crée lui-même des bandes latérales à F_in, la sortie est sous ce plancher |
| Voix soufflée ; 96 et 384 kHz | toujours au-dessus du plancher de la voix idéale |
| Changement de note G3 −35 c -> C#4 +30 c (glissé 50 ms) | ancienne note rejetée à 61 dB, note chantée à 45–48 dB (idéale 40 dB) |
| Transmission harmonique par harmonique, 100 Hz – 19,5 kHz, Preserve OFF | ±0,05 dB à 48, 96 et 384 kHz (objectif ±0,2 dB) |
| Voix réelle, Preserve ON, tiers d'octave 4–16 kHz | ±0,42 dB Live, ±0,35 dB Studio (la bande 2,5 kHz, +0,8 / +1,1 dB, mesure la précision des formants, pas un voile) |
| Flow rapide, 8 syllabes, glissés 25 ms, consonnes, vibrato | Studio 100 % des trames à ±8 c (max 4,3 c) · Live hors vibrato 100 % à ±8 c sous Linux (max 7,2 c ; 8,9 c sous macOS ; critère ≥ 95 % et max < 12 c), la syllabe en vibrato à 16,9 c |
| Stéréo Linked, voix centrée | \|L − 0,7 R\| = 1·10⁻¹⁶ : mêmes marques, même phase |

**Limite Live** : le détecteur a 13,5 ms de retard ; un vibrato de ±30 c aplati à Retune 0 laisse un résidu de cet
ordre (17 c). Studio, qui lit la commande dans le futur, n'a pas ce résidu.

## Étape 5A — moteur expressif et ergonomie

### Transition / Flex (`CorrectionCurve`, `ScaleQuantizer.h`)

Paramètre `transition`, 0 … 200 ms, affiché **FLEX**. Distinct du Retune (vitesse de maintien sur la note) :

- quand la voix bouge vite (plus de 6 dt/s mesurés sur 10 ms : glissando, slide d'attaque, inflexion), la correction
  est **figée à sa valeur d'avant le mouvement** : la sortie suit la voix telle qu'elle est chantée, sans escalier ;
- dès que la voix se pose (moins de 3 dt/s, hystérésis), ou à une nouvelle attaque, la correction rejoint la note
  cible en `transition` ms (courbe en S), puis la note est tenue à la vitesse Retune ;
- 0 ms : rien n'est calculé, comportement exact de l'étape 4.

Une première version figeait la correction au moment où la voix changeait de note (au milieu du glissé) : le
hard-tune y corrige déjà de −50 c, et la sortie restait décalée de 50 c pendant tout le glissé. Le banc l'a montré
(écart de 54 c, contre 10 c une fois l'ancrage placé au début du mouvement).

### MIDI Target (`ScaleQuantizer::quantizeToNotes`, `AuraPitchEngine`, `PluginProcessor`)

- `NEEDS_MIDI_INPUT TRUE`, `acceptsMidi() = true`. L'AU devient un *Music Effect* (`aumf`) : c'est le type qui reçoit
  du MIDI dans Logic ; `auval -v aumf Aupt Nxds` dans la CI.
- NoteOn / NoteOff (NoteOn de vélocité 0 = NoteOff), Pitch Bend (±2 dt), All Notes Off / All Sound Off, tous canaux.
  Chaque évènement est horodaté à l'échantillon (`addMidiEvent (ev, offsetDansLeBloc)`) et rangé dans une file
  statique de 512 évènements.
- Live applique l'état MIDI de l'instant traité ; Studio celui du centre de la trame analysée (même alignement que la
  voix, la latence Studio est donc aussi celle du MIDI).
- Cible = note tenue **la plus proche** de la voix, en hauteur absolue (une voix en A3 avec C4 tenu va à C4, pas à
  C3), même hystérésis de 12 c que la gamme entre les notes d'un accord, plus le Pitch Bend. Aucune note tenue : la
  gamme reprend la main.
- Les notes tenues sont visibles par l'interface sans délai (atomiques) : touches et lignes de la grille en ambre.

### Auto-Key (`AutoKeyDetector.h`)

- Pendant l'écoute, chaque trame voisée ajoute à sa classe de hauteur le poids durée × clarté × niveau, réparti entre
  les deux demi-tons voisins selon l'écart en cents.
- Profils de Temperley (version améliorée de Krumhansl-Schmuckler) pour les 24 tonalités. Décision bayésienne :
  log P(clé) = N · Σ h(i) log p_clé(i), avec N = 4 notes par seconde voisée. La **confiance** est la probabilité a
  posteriori de la meilleure tonalité : elle monte avec la durée d'écoute et avec l'écart à la concurrente
  (souvent la relative : Si mineur / Ré majeur).
- Commandes par atomiques (interface -> audio) ; analyse toutes les 100 ms sur le thread audio (24 × 12 opérations).
- Interface : **AUTO-KEY** -> « LISTENING… » puis « Bm 72%… » en direct -> appliqué dès 90 % après au moins 4 s de
  voix (« B MINOR (94%) » affiché 6 s). Second clic : arrêt et application si ≥ 60 %. 30 s sans verdict : arrêt.

### Throat (`ThroatModel`, `FormantProcessor.h`)

- Analyse : somme mono du Wet, décimée vers 11–12,6 kHz (demi-bandes), trames de 25 ms toutes les 5 ms, LPC d'ordre
  14 (pré-accentuation 0,94, fenêtre de décalage gaussienne de 20 Hz), racines par Durand-Kerner. Jusqu'à 4 formants
  (150–4800 Hz, B < 600 Hz), appariés d'une trame à l'autre par proximité en log-fréquence, lissés 12 ms.
- Déformation : f' = f · τ^(−g(f)), g = 1 jusqu'à 3,5 kHz puis demi-cosinus vers 0 à 5 kHz.
- Filtre : une cellule par formant, zéros sur (f, B), pôles sur (f', B·f'/f) — même Q, la résonance est étirée comme
  l'enveloppe —, gain unité à 0 Hz. C'est exactement E(f·τ)/E(f) sous 3,5 kHz pour un conduit tous-pôles.
- Pourquoi un croisement : les formants déplacés changent aussi la pente 1/f² au-dessus d'eux (±13 à 16 dB à
  8 kHz pour τ = 80 / 120 %). Les cellules ne filtrent donc que la voie basse d'un croisement **doublement
  complémentaire** (Butterworth d'ordre 9 à 4,6 kHz, LP = (A1 + A2)/2, HP = (A1 − A2)/2, LP + HP = A1 passe-tout).
  Au-dessus de 6 kHz la sortie est la voix elle-même. Sans latence, à la fréquence native.
- τ = 100 % : `isIdle()`, rien n'est calculé. Sinon le Wet entier porte la phase du passe-tout A1 (module plat), et
  le Dry passe par le même passe-tout : le Mix < 100 % reste sans peigne. Engagement / retrait en 20 ms ; le bypass
  retire aussi le Throat (Dry retardé exact, vérifié au banc du plug-in).
- Non voisé : les formants reviennent à l'identité en 10 ms (Wet = Dry à la phase du passe-tout près).

Une première version utilisait des cellules sans normalisation : déplacer un pôle change son gain aux basses
fréquences, et la voyelle prenait +10 à +12 dB sur h1–h2 à τ = 120 %.

| Mesure (voyelle source-filtre F1–F5, F0 196 Hz, 44,1 / 48 / 96 / 384 kHz, Live et Studio) | τ 80 % | τ 90 % | τ 110 % | τ 120 % |
|---|---|---|---|---|
| Déplacement des formants mesuré (attendu) | +4,05 dt (+3,86) | +1,95 (+1,82) | −1,65 (−1,65) | −3,15 (−3,16) |
| Écart RMS à la déformation idéale E(w⁻¹f)/E(f), 150 Hz – 3,3 kHz | 1,4 dB | 1,3 dB | 2,2–2,4 dB | 3,0–3,3 dB |
| Fondamentale (graves) | −0,03 dB | −0,01 dB | +0,02 dB | +0,03 dB |
| Sons purs 7 / 9 / 12 kHz | ±0,08 dB | ±0,01 dB | ±0,01 dB | ±0,01 dB |

L'écart RMS restant vient de l'estimation des bandes passantes (LPC sur un spectre harmonique : F4 estimé à 293 Hz
pour 180) ; un ordre LPC plus élevé s'accroche aux harmoniques (8 dB d'écart à l'ordre 18).

### Doubler

- Deux têtes à raccords glottiques (`SpliceShifter`) sur la somme mono du Wet : gauche −7 c, droite +7 c, retards
  moyens ≈ 9 et 14 ms (la tête garde son retard dans [D, D + 1,25 T] : D = 7,5 / 12,5 ms).
- Au-delà de 96 kHz, les doublures tournent à ≤ 96 kHz (÷2^k, raccords, ×2^k, retard de la chaîne mesuré et retiré) :
  les voix doublées n'ont pas besoin de l'air du DXD. Coût à 384 kHz : ~6 % -> ~3 % d'un cœur.
- Mélange : Wet ± 0,707·d·doublure, niveau global conservé (1/√(1 + g²), voix décorrélées). Stéréo seulement (moteur
  lié) ; 0 % : rien n'est calculé.

### Éditeur

- `KnobSlider` : glissé de 220 px pour toute la course ; Maj, Cmd (macOS) ou Ctrl (Windows / Linux) divisent la
  sensibilité par 5. Le calcul est incrémental, on peut donc changer de modificateur en cours de geste sans saut.
  Double-clic (ou Alt-clic) : valeur par défaut. Clic sur la valeur ou clic droit : saisie au clavier (« 3.5 »,
  « -2.5 »).
- **FREEZE** dans l'en-tête du graphe : l'affichage se fige, la FIFO continue d'être vidée (aucun débordement).
- Panneau gamme : **AUTO-KEY** et **MIDI** (paramètre `midi_target`, automatisable). Rangée du bas : 10 boutons +
  Preserve en quatre groupes séparés — correction (Retune, Flex, Humanize), timbre (Formant, Preserve, Throat),
  hauteur (Transpose, Fine Tune, A4), sortie (Doubler, Mix).

### Banc `aurapitch_step5a_test` (209 vérifications)

| Mesure | Résultat |
|---|---|
| Auto-Key, mélodie en Si mineur (Bandit) et en Do majeur, voix réaliste ±18 c, 48 et 384 kHz, Live et Studio | B Minor 99,8 % · C Major 99,9 % ; 3 notes (1,2 s) : 36 % |
| MIDI : voix A3 + NoteOn C4, Pitch Bend +1, NoteOff, 48 / 96 / 384 kHz | C4 ±0,01 c (max 0,14) · D4 ±0,01 c · retour A3 ±0,01 c |
| MIDI : accord C4-E4-G4, voix E4 +60 c | E4 −0,01 c |
| Flex : glissando A3 -> C4 en 150 ms, hard-tune | Transition 0 : escalier (55 c d'écart) · 120 ms : la sortie suit la voix (10 c = justesse d'origine), puis C4 à 0,0 c |
| Throat | voir le tableau ci-dessus |
| Doubler 100 % | gauche −7,00 c, +9,5 ms · droite +7,00 c, +14,5 ms ; mono inchangé |
| Throat 100 % / Doubler 0 % / Transition 0 / MIDI Off | identique au bit près ; retour au bit près après une excursion |
| Temps réel, toutes les fonctions actives, automation à chaque bloc, Live <-> Studio | 0 allocation, 44,1 / 96 / 384 kHz |

Charge CPU, stéréo, voix corrigée (même machine que l'étape 2), réglages neutres -> Throat 115 % + Doubler 60 % :

| fs (kHz) | 48 | 96 | 192 | 384 |
|---|---|---|---|---|
| Live | 1,6 -> 3,0 % | 2,3 -> 5,0 % | 3,9 -> 7,8 % | 7,4 -> 13,9 % |
| Studio/DXD | 4,9 -> 6,4 % | 5,9 -> 9,0 % | 6,4 -> 10,6 % | 8,5 -> 15,0 % |

### Limites de l'étape 5A

- **Throat en Mix < 100 % ou en bypass progressif** : quand Throat ≠ 100 %, toute la sortie (Wet et Dry) porte la
  phase du passe-tout de croisement (module plat) ; pendant les 20 ms d'engagement ou de retrait, Wet direct et Wet
  passe-tout se mélangent (léger creux autour de 4,6 kHz, le temps du fondu).
- **Doubler en double mono** : le doubleur travaille sur le moteur lié (stéréo). En Dual Mono, chaque moteur est mono
  et le doubleur ne fait rien.
- **Flex et vibrato** : un vibrato rapide dépasse le seuil de 6 dt/s ; avec Flex > 0 il passe en partie (c'est
  l'intention : l'expression passe). Pour un vibrato aplati, Flex à 0.
- **Auto-Key** : majeur / mineur naturel seulement (pas de modes) ; la confiance suppose ≈ 4 notes par seconde.
- **AU** : le changement de type (`aufx` -> `aumf`) oblige à réinsérer l'AU dans une session Logic existante.
- **Budgets CPU des bancs** : stricts en local (5 % étape 1, 25 % étape 2, 25 % étape 5A) ; la CI les multiplie par 3
  (`AURAPITCH_CPU_SCALE`), car un runner partagé est 2 à 3× plus lent et sa charge varie (un passage Windows a
  échoué au budget de 5 % de l'étape 1 sur un commit de documentation).

## Étape 5B — GRAPH EDITOR chirurgical, calé sur la timeline du DAW

### Principe : aucune donnée audio stockée

L'audio continue de traverser le moteur en direct (même chaîne qu'aux étapes 4 / 5A). Le plug-in ne garde que des
métadonnées datées sur la timeline du DAW :

1. `setTimeline (secondes, lecture)` : le processeur transmet à chaque bloc la position de l'`AudioPlayHead`
   (`timeInSeconds`, sinon `timeInSamples`). Le moteur garde les 256 derniers segments (horloge interne -> secondes)
   et date chaque trame d'analyse au centre de sa fenêtre.
2. **CAPTURE** : pendant la lecture, chaque trame (temps, hauteur MIDI, voisement, niveau) part dans une
   `CaptureFifo` SPSC statique ; le timer du message thread (20 Hz) la vide dans le `NoteGraphModel`. Une passe qui
   repasse sur une zone déjà capturée la remplace. 16 octets par trame, ≈ 670 trames/s : ≈ 50 Ko pour 3 s, quelle
   que soit la fréquence d'échantillonnage.
3. **Rejeu** : là où une note couvre l'instant t,
   hauteur voulue = cible + vibratoDepth × écart(t) + glissé(t), hauteur chantée = centre + écart(t) (mesurée au
   passage de capture), décalage = différence. Il ne dépend pas du détecteur : pas de retard, même en Live.

### Segmentation en notes (`NoteGraphModel::segment`)

- Passages voisés (trous < 40 ms comblés) ; un passage n'est segmenté qu'une fois dépassé de 80 ms (capture en cours).
- Hauteur lissée sur 150 ms (≈ une période de vibrato : il s'efface), notes par hystérésis de 0,6 dt ; les segments
  de moins de 60 ms, et ceux d'un glissé (hauteur entre leurs deux voisins, < 150 ms), rejoignent leurs voisins.
- Frontière : là où la hauteur, lissée sur 20 ms, franchit le milieu des deux notes (liaison legato).
- `NoteBlob` : début, durée, centre (moyenne des 60 % centraux), cible (note de la gamme la plus proche),
  `vibratoDepth` 1, `transitionInRatio` 0, `formantOffsetSemitones` 0, micro-courbe (écart au centre, cents) et
  enveloppe de niveau à 200 Hz, chaque point étant la moyenne des trames de son pas de 5 ms.

Deux défauts trouvés au banc et corrigés : un passage pouvait être tronqué en segmentation incrémentale (la recherche
partait 1 s avant la zone modifiée, au milieu d'un passage : note 2 commençant à 1,207 s au lieu de 0,95 s) ; le
glissé legato C4 -> E4 créait une note de 30 ms (C#, D, D# traversés par la hauteur lissée).

### Injection temps réel (`AuraPitchEngine`, `GraphExchange`)

- Le thread audio ne lit qu'un `GraphSnapshot` immuable (notes triées, micro-courbes concaténées, recherche
  dichotomique). Le message thread publie un nouveau snapshot par échange atomique ; l'ancien revient au message
  thread par une file SPSC de 64 places et y est libéré. Le thread audio n'alloue, ne libère et ne verrouille jamais
  (si la file de retour est pleine, il garde l'ancien snapshot un bloc de plus).
- **Live** : sous une note, le rapport est recalculé tous les 32 échantillons pour l'instant d'entrée que la tête de
  lecture lit vraiment (position − retard de lecture du `SpliceShifter` + 1 ms d'avance pour le lissage du rapport) ;
  la période des raccords vient aussi de la hauteur capturée.
- **Studio** : la piste horodatée reçoit le décalage de la note à l'instant de chaque trame ; le filtre de formants
  reçoit la hauteur capturée.
- Formant par note : ajouté au FormantShift global (lissé 30 ms) ; s'il existe des notes à formant, le cœur PSOLA
  prend la main (fondu de 10 ms), comme pour un FormantShift global.
- Hors des notes, hors lecture, ou pendant une capture : correction automatique habituelle.

### Sauvegarde

`NoteGraphModel::serialize()` : binaire compact (« APGM », version, notes en doubles exacts, courbes en float),
stocké en base64 dans l'état de session (propriété `noteGraph`). La copie sérialisée est tenue à jour par le message
thread : un hôte qui appelle `getStateInformation()` depuis un autre thread ne lit jamais le modèle en cours d'édition.
2 076 octets pour une note scindée en deux (1,2 s de chant) ; 3,9 Ko d'état complet.

### Interface (`GraphEditor`)

- En-tête du panneau de hauteur : **MONITOR | GRAPH**, puis en vue GRAPH **CAPTURE**, **CLEAR**, **QUANTIZE ALL**,
  **FOLLOW** ; zoom **− / +** au bout de la règle.
- Piano-roll : rangées de demi-tons (gamme active éclairée, tonique marquée), règle en secondes et mesures (tempo de
  l'hôte, 4/4), capsules cyan dont l'épaisseur suit le niveau, courbe chantée en pointillés ambre, courbe rejouée en
  trait lumineux (elle s'aplatit quand le vibrato baisse, s'incline sur les glissés), poignées (gauche : glissé ;
  haut : vibrato), étiquette (note, cents, vibrato, formant), tête de lecture, trames brutes en rouge pendant la capture.
- Souris : voir l'en-tête de `GraphEditor.h` (glisser au demi-ton aimanté sur la gamme, Alt / Maj au cent, lasso,
  double-clic = quantifier, Alt + double-clic ou clic droit = scinder, Cmd/Ctrl = vibrato, Cmd/Ctrl + Alt = formant,
  poignée gauche = glissé, molette / Maj + molette = défilement, Cmd/Ctrl + molette = zoom temps, + Alt = zoom hauteur).

### Banc `aurapitch_step5b_test` (66 vérifications ; 48 sans JUCE)

| Mesure | Résultat |
|---|---|
| Capture, phrase A3 · C4 ~legato~ E4 · G4, vibrato ±30 c, jitter, timeline à 10 s, 48 / 384 kHz, Live / Studio | 4 notes, frontières à ±9 ms (legato à 1,601–1,602 s pour 1,60), centres à ±2,6 c, cibles justes ; 2 116 trames (50 Ko) |
| Note 2 (C4 −20 c, vibrato ±30 c) -> D4, vibrato 0, second passage | D4 à ±0,06 c ; vibrato 27,5 -> 1,0 c (−96,0 à −96,6 %) ; note 3 juste, vibrato 27,6 c conservé |
| Transparence (protocole de l'étape 4, voix A3 +40 c, Preserve OFF, corrigée par une note) | harmoniques 1 … 87 à ±0,030 – 0,049 dB ; identique à la correction automatique au même rapport à ±0,001 dB |
| Frontière de deux notes éditées (legato D4 / E4), énergie > 6 kHz d'une voyelle sombre | −70,1 dB, contre −70,3 dB dans les notes : aucun clic |
| Scission C#4 -> C4 puis E4, sérialisation, nouveau modèle + nouveau moteur | C4 / E4 à ±0,16 c ; rendu identique au bit près |
| Temps réel, rejeu et capture pendant 50 à 259 publications concurrentes (thread d'interface simulé) | 0 allocation sur le thread audio ; ASan / UBSan / TSan muets |
| Sans notes / transport arrêté | identique au bit près |
| Processeur : AudioPlayHead simulé, CAPTURE, scission, formant +2 dt, getState -> nouvelle instance -> setState | rendu identique au bit près (48 / 384 kHz) |
| Interface : glisser, Alt, double-clic, Cmd, Cmd + Alt, poignée gauche, clic droit, lasso, Cmd + molette | 62,00 / 62,37 / 62,00 / vibrato 0 % / formant +2,0 / glissé 50 % / 5 notes / sélection / zoom ; rejoué : D4 −0,05 c |

### Limites de l'étape 5B

- **Résidu de vibrato ≈ 1 c (−96 %)** : la fenêtre d'analyse du détecteur atténue légèrement le vibrato mesuré à la
  capture ; la correction compense donc un peu moins que le vibrato réel. Le critère (> 95 %) passe avec ~1 % de marge.
- **Transparence aux grands rapports** : la chaîne à raccords (étape 4) perd 0,14 dB à 19 kHz à +2 dt et jusqu'à
  2,4 dB à +5 dt à 48 kHz (bande de transition du banc anti-repliement) — en correction automatique comme en GRAPH.
  Le ±0,05 dB est tenu sur le protocole de l'étape 4 (+40 c).
- **La capture suppose que l'audio rejoué est celui qui a été capturé** : après une nouvelle prise, il faut
  recapturer la zone (CAPTURE la remplace).
- **Undo** : les éditions du GRAPH ne passent pas par l'undo de l'hôte.
- **Dual Mono** : la capture vient du canal gauche ; les mêmes notes s'appliquent aux deux moteurs.
- **AU / VST3 non testés dans un DAW réel** (Logic, Cubase, Reaper) : l'AudioPlayHead est simulé au banc.


## Étape 6 — VST3 seul, GRAPH EDITOR agrandi, AuraKey

### VST3 uniquement
- `FORMATS VST3` pour AuraPitch et AuraKey ; `AU_MAIN_TYPE` et la liste de formats conditionnelle sont supprimés. Plus
  aucun `.component` ni `.app` n'est généré (vérifié : l'arborescence de build ne contient que les deux `.vst3`).
- CI : plus d'`auval` ni d'archive AU / Standalone ; `lipo -info` et archives pour les deux `.vst3`.

### GRAPH EDITOR agrandi (`GraphWindow`)
- Bouton à double flèche au bout de l'en-tête du GRAPH EDITOR (`ExpandButton`, icône « réduire » quand la fenêtre est
  ouverte). Le **même** composant `GraphEditor` est reparenté dans une `DocumentWindow` native (1200 × 800 centrée
  sur l'écran du plug-in, redimensionnable, limites 640 × 400, boutons réduire / agrandir / fermer de l'OS) : vue,
  sélection, zoom et glissés en cours sont conservés ; aucune copie du modèle.
- Dans l'éditeur, un panneau de renvoi remplace la vue (SHOW WINDOW, RETURN HERE). Le sélecteur MONITOR | GRAPH reste
  utilisable ; le GRAPH EDITOR continue d'être rafraîchi (`tick`) tant que la fenêtre est ouverte.
- Fermeture (bouton de la fenêtre, Échap, bouton réduire) : la fenêtre est détruite hors de son propre rappel
  (`MessageManager::callAsync` + `SafePointer`), le composant revient dans l'éditeur à sa place. La position de la
  fenêtre est mémorisée par le processeur (pas dans l'état du projet). Fermer le plug-in avec la fenêtre ouverte
  détruit la fenêtre avant le composant et le LookAndFeel.
- macOS : la fenêtre est « toujours devant » pour ne pas passer derrière la fenêtre (souvent flottante) du plug-in.
- Interface seulement : rien de nouveau n'est partagé avec le thread audio (les éditions passent toujours par
  `graphEdited` -> `GraphExchange`, sans verrou).
- Banc `aurapitch_step5b_test`, test **[9] expand** (17 vérifications, sous Xvfb) : ouverture 1200 × 800 sur le
  bureau, composant reparenté (même objet), espace ×2,0 en largeur et ×3,6 en hauteur de rangée, vue conservée,
  glisser + scinder dans la fenêtre, redimensionnement suivi, fermeture par le bouton de la fenêtre (rappel
  asynchrone), retour à la place exacte, éditions conservées et rejouées (note 3 à −0,14 c de la cible), réouverture
  à la dernière position, fermeture de l'éditeur avec la fenêtre ouverte.

### AuraKey — chaîne d'analyse
```
bloc audio (float / double, 1 ou 2 canaux) ── sortie = entrée, au bit près
   └─ mixage mono (double) -> Decimator (≈ 11 kHz) -> ChromaAnalyzer (FFT 8192, pas 2048 ≈ 0,19 s)
         └─ pics 75 Hz … 2 kHz -> chroma 36 cases + écarts d'accord -> repli 12 classes (accord estimé)
               └─ AutoKeyDetector (étape 5A : Temperley, bayésien) -> clé, confiance, 24 probabilités
```
- **Decimator** : facteur entier round(fs / 11025), RIF sinc fenêtré Blackman à 16·D + 1 coefficients, coupure
  0,42 × fa ; bande utile plate à ±0,002 dB jusqu'à 2 kHz, repliement −90 dB à toutes les fréquences (44,1 … 384 kHz).
- **ChromaAnalyzer** : maximum local au-dessus de −60 dB du maximum et de 1,6 × la moyenne locale ; fréquence et
  amplitude par interpolation parabolique du log-module ; poids √amplitude ; répartition linéaire entre cases voisines
  (comme l'Auto-Key de 5A pour une voix fausse). Accord = moyenne circulaire des écarts au demi-ton (ignorée si la
  résultante est < 0,25). Poids de trame = durée × niveau (−60 dBFS -> 0, −20 dBFS -> 1).
- **AutoKeyDetector** (partagé) : nouvelles méthodes `addPitchClassWeights`, `setEvidenceRate` (1,5 « notes » par
  seconde pour un chroma polyphonique, contre 4 pour une voix), `decay`, `posteriors`. Les valeurs par défaut gardent
  le comportement de l'étape 5A (banc 5A inchangé, 209 / 209).
- **Live** (`LiveKeyAnalyzer`) : mémoire τ = 8 s, 30 s (défaut) ou ∞ par atténuation e^{−dt/τ} à chaque trame avec du
  contenu (les silences ne font pas oublier) ; accord sur 60 s glissantes ; RESET par drapeau atomique. L'état publié
  (≈ 330 octets) passe à l'interface par un **seqlock** à mots atomiques : l'audio n'attend jamais, l'interface relit
  si une écriture l'a croisée.
- **Fichier** (`OfflineKeyAnalyzer`, fil `AnalysisJob` de priorité basse) : lecture par blocs de 64 k échantillons
  (`AudioFormatReader`, pas de fichier entier en mémoire : seules les trames de chroma — 36 flottants / 0,19 s — et
  l'enveloppe d'attaques sont conservées), puis :
  - accord du morceau et clé globale sur tout le chroma ;
  - **tempo** : flux spectral positif (FFT 512, pas 64 ≈ 172 trames/s), moyenne locale retirée, lissage gaussien,
    autocorrélation jusqu'à 4 s, peigne sur 4 multiples de la période pondéré par un a priori log-normal centré sur
    120 BPM (σ = 1 octave), affinage au millième de BPM ;
  - **sections** : a posteriori des 24 clés sur une fenêtre de 7 s, une valeur par seconde, Viterbi (changer de clé
    coûte 18 nats) ; sections de moins de 6 s rattachées à leur voisine.
- Formats : `registerBasicFormats` (WAV, AIFF, FLAC, Ogg ; Core Audio sous macOS) + décodeur MP3 logiciel de JUCE
  (`JUCE_USE_MP3AUDIOFORMAT`, option `AURAKEY_MP3_DECODER`, avertissement de licence de JUCE à lire).
- Un nouveau fichier (ou CANCEL) annule l'analyse en cours au bloc suivant. Fichier illisible : message, résultat
  précédent conservé. Le résultat (clé, tempo, accord, sections, chroma, 24 probabilités, caractéristiques du
  fichier) est sauvegardé dans l'état du plug-in, pas le fichier.

### AuraKey — interface
- 1000 × 640 (840 × 560 … 1800 × 1200). LIVE KEY : clé « G Major », confiance « 98% », Camelot, relative, durée
  écoutée, accord (et A de référence), niveau d'entrée, classes de hauteur (notes de la gamme en couleur) ; cercle
  des quintes ; MEMORY (8 s · 30 s · ∞) et RESET. FILE ANALYSIS : zone de dépôt (survol en surbrillance), barre de
  progression et CANCEL pendant l'analyse, puis clé « D Major (100%) », tempo, Camelot, relative, accord, classes de
  hauteur, format / bits / fréquence (DXD) / canaux / durée / temps d'analyse. SECTIONS : bandes cyan (majeur) /
  violet (mineur), règle mm:ss, survol détaillé, clic = clé épinglée sur le cercle.
- Cercle des quintes : intensité = √(probabilité / max) × contraste (rien d'allumé tant que les 24 clés sont
  équiprobables) ; clé live lumineuse, clé du fichier en ambre, clé épinglée en blanc ; centre : clé, Camelot,
  probabilité live, notes de la gamme.

### Banc `aurakey_test` (159 vérifications ; 72 sans JUCE)
| Test | Mesure |
|---|---|
| [1] décimateur | 44,1 / 48 / 96 / 192 / 352,8 / 384 kHz : bande utile ±0,002 dB, repliement −89,7 à −91,7 dB |
| [2] live, 30 s, mémoire ∞ | D, B mineur (384 kHz), F mineur, E, Ab (sans batterie), C désaccordé +35 c, G mineur −40 c (88,2 kHz) : bonne clé, confiance ≥ 99,6 %, accroche en 4,6 à 6,5 s, accord estimé à ±0,5 c ; Am F C G : C majeur (relative, acceptée : voir limites) |
| [3] mémoire | 8 s : F mineur 14,8 s après une modulation D majeur -> F mineur ; ∞ : garde l'historique ; RESET vide l'historique |
| [4] temps réel | 48 et 384 kHz, blocs irréguliers 64 … 544 : 0 allocation, CPU 0,10 % / 0,59 % d'un cœur, 168 k / 897 k lectures concurrentes de l'état, 0 incohérente |
| [5] hors ligne | D 128 BPM, B mineur 96 BPM (DXD), Eb 140 BPM, C# mineur 75 BPM −25 c : bonne clé, tempo à ±0,14 BPM, accord ±0,6 c, une section ; D -> F mineur -> A : trois sections, limites exactes (36 s, 72 s) ; silence : ni clé ni tempo |
| [6] processeur | 48 kHz 32 bits, 384 et 44,1 kHz 64 bits : sortie identique à l'entrée au bit près, 0 allocation, B mineur ≥ 99,1 % ; bus mono ; MEMORY dans l'état |
| [7] fichiers | WAV 16 bits 44,1 k, WAV 32 bits flottants 384 k, WAV 24 bits 352,8 k, AIFF 24 bits 96 k, FLAC 24 bits 48 k (modulation G -> E mineur), FLAC 24 bits 384 k, MP3 192 kb/s (ffmpeg) : clé, tempo ±0,5 BPM, sections, fréquence et durée ; analyse de 0,11 à 0,38 s par fichier pendant que le thread audio tourne (23 k à 75 k blocs, 0 allocation, transparent) ; annulation d'un DXD de 2 min en 2 ms ; fichier illisible ; état restauré à l'identique |
| [8] interface | survol refusé (.txt) / accepté (.flac), surbrillance, dépôt -> analyse (D / B mineur / D : 3 sections), 24 cases du cercle, survol, épingler / désépingler, clic sur une section -> B mineur épinglée, RESET, capture PNG |

### Limites de l'étape 6
- **Morceaux de synthèse seulement** : aucun enregistrement réel n'était disponible dans l'environnement. Sur de la
  musique réelle (mixages denses, voix, distorsion, basses désaccordées), les détecteurs de clé à profils se trompent
  surtout entre **relatives** (Am / C) et entre **quintes voisines** ; c'est visible ici sur la progression Am F C G,
  détectée C majeur (ce sont exactement les accords de C majeur : seul l'ordre la dit mineure, et un histogramme ne
  voit pas l'ordre), et avec **99 % de confiance** (A mineur : 0,7 %) : la confiance mesure l'accord avec les
  profils, pas l'ambiguïté musicale.
- **Tempo** : un seul tempo global (pas de carte de tempo) ; les ambiguïtés d'octave (87 / 174 BPM en drum'n'bass,
  ternaire) sont départagées par l'a priori à 120 BPM, pas résolues.
- La confiance live dépend de `kChromaEvidenceRate` (1,5), calibré sur ces morceaux de synthèse : elle monte vite
  (> 99 % en 30 s) et devra peut-être être ralentie sur de vrais morceaux.
- Fenêtre agrandie : testée sous Xvfb (Linux) seulement ; le comportement « toujours devant » sous macOS et la
  cohabitation avec les fenêtres flottantes des DAW restent à vérifier dans Logic / Ableton / Cubase.
- Les deux plug-ins n'ont pas été chargés dans un DAW réel, et la CI GitHub n'a pas tourné (quota macOS).
