# AuraPitch 0.1 — NextDSP · architecture (étape 1)

Correction de hauteur et Pitch & Formant Shifter, VST3 / AU / Standalone, C++20 · JUCE 8.0.15 (même version
qu'AuraEngine, AuraDAW et AuraRack). Traitement en `double` de 44,1 kHz au DXD 352,8 / 384 kHz, entrée/sortie
`float` ou `double` selon l'hôte.

**État** : étape 1 livrée et testée — `ScaleQuantizer.h` (+ `CorrectionCurve`), `PitchDetector.h`,
`LockFreePitchFifo.h`, `CMakeLists.txt`, banc `aurapitch_dsp_test` (247 / 247). Étape 2 : moteurs de resynthèse.
Étape 3 : `PluginProcessor`, `PluginEditor`.

## Arborescence

```
AuraPitch/
├── CMakeLists.txt                     JUCE 8.0.15 (FetchContent ou AURAPITCH_JUCE_DIR) ; noyau DSP sans JUCE
├── Source/
│   ├── DSP/                           en-têtes C++20 sans JUCE, testables seuls
│   │   ├── PitchDetector.h            ✅ décimation demi-bande + MPM + raffinement ×4 + voisement
│   │   ├── ScaleQuantizer.h           ✅ gammes, clavier Custom (Active / Bypass / Removed), A4 415–466,
│   │   │                                 CorrectionCurve (Retune Speed log, Humanize, Transpose, FineTune)
│   │   ├── LockFreePitchFifo.h        ✅ SPSC statique audio -> UI (PitchFrame)
│   │   ├── PSOLAShifter.h             étape 2 — TD-PSOLA Live : marques de pitch, grains Hann 2 périodes
│   │   ├── PhaseVocoderShifter.h      étape 2 — Studio/DXD : vocodeur de phase, verrouillage de phase
│   │   ├── FormantProcessor.h         étape 2 — enveloppe cepstrale (lifter), FormantShift, FormantPreserve
│   │   └── LatencyCompensatedDry.h    étape 2 — ligne de retard Dry = latence du chemin Wet
│   ├── Parameters.h/.cpp              étape 3 — disposition APVTS, identifiants figés
│   ├── PluginProcessor.h/.cpp         étape 3 — AuraPitchProcessor
│   └── GUI/                           étape 3 — AuraPitchEditor, LookAndFeel NextDSP, PitchGraph,
│                                                ScaleKeyboard, CentsMeter
└── Tests/
    └── PitchDspSelfTest.cpp           ✅ banc DSP (précision, voisement, CPU, allocations, FIFO)
```

Tant que `Source/PluginProcessor.cpp` n'existe pas, CMake ne construit que le noyau DSP et le banc.

## Flux du signal

```
               ┌──────────────── thread audio (processBlock<float|double>) ─────────────────────────────────┐
 entrée L/R ─► │ somme mono ─► PitchDetector ──(toutes les 1,5 ms)──► ScaleQuantizer ─► CorrectionCurve      │
               │                     │ période, frameCentre               │ cible, cents      │ décalage (½ t) │
               │                     ▼                                    ▼                   ▼                │
               │              PSOLAShifter (Live) ou PhaseVocoderShifter (Studio/DXD) ◄── ratio lissé          │
               │                     │            ▲ FormantProcessor (enveloppe, FormantShift / Preserve)      │
               │                     ▼                                                                         │
               │  Dry retardé (LatencyCompensatedDry) ──► Mix ─► sortie                                        │
               │  PitchFrame ─► LockFreePitchFifo ─────────────────────────────────────────────┐               │
               └────────────────────────────────────────────────────────────────────────────────┼──────────────┘
                                                                                                ▼
                                       message thread : AuraPitchEditor (timer VBlank 60/120 Hz) drain()
```

## Classes

### `aura::pitch::PitchDetector` (✅)

| Méthode | Thread | Rôle |
|---|---|---|
| `prepare (double fs)` | prepareToPlay | choisit la décimation, alloue pour 50 Hz (pire cas) |
| `setInputType / setRange / setHopSeconds / setConfig` | audio | sans allocation |
| `process<T> (const T*, int n, cb)` | audio | `cb (const PitchEstimate&, int indexDansLeBloc)` à chaque estimation |
| `getAnalysisLatencySamples()` | tous | retard entre l'entrée et l'instant décrit (`frameCentre`) |

`PitchEstimate` : `frequencyHz`, `periodSamples` (à fs, pour les marques PSOLA), `clarity`, `levelDb`,
`brightnessHz`, `voiced`, `frameCentre` (horloge d'échantillons absolue).

**Décimation intelligente.** Cascade de demi-bandes ÷2 (47 coefficients, 12 multiplications distinctes, réjection
≈ 96–104 dB, bande plate à 0,000 dB) jusqu'à une fréquence d'analyse fsA ∈ [8 ; 16[ kHz :

| fs | 44,1 | 48 | 88,2 | 96 | 176,4 | 192 | 352,8 | 384 |
|---|---|---|---|---|---|---|---|---|
| ÷ | 4 | 4 | 8 | 8 | 16 | 16 | 32 | 32 |
| analyse / raffinement (Hz) | 11 025 / 44 100 | 12 000 / 48 000 | 11 025 / 44 100 | 12 000 / 48 000 | idem | idem | 11 025 / 44 100 | 12 000 / 48 000 |

La cascade coûte moins de 13 multiplications par échantillon d'entrée quel que soit fs ; le MPM et le raffinement
travaillent toujours sur le même nombre d'échantillons. La charge est donc la même à 44,1 kHz et à 384 kHz.

**Précision.** MPM (NSDF, maxima clés, k = 0,93, interpolation parabolique) à fsA, puis raffinement à 4 × fsA sur
±4 retards, sur un signal limité à 0,35 fsA (Butterworth 4) pour ne pas importer le bruit de souffle.

**Voisement.** Gate RMS (−60 dBFS), clarté NSDF avec hystérésis (0,80 / 0,70), et brillance
fEq = fsR / 2π · acos ρ1 (fréquence RMS du spectre) : une trame plus brillante que max(3 kHz, 2,5 × F0) et de
clarté < 0,95 est une sifflante ou une plosive sourde. Le seuil de clarté forte garde voisés les synthés et cuivres.

**Latence d'analyse** (fenêtre = 2 périodes de la F0 la plus grave de l'InputType) :

| InputType | Plage F0 | Latence |
|---|---|---|
| Soprano | 150–1200 Hz | 6,9 ms |
| Alto/Tenor | 75–900 Hz | 13,5 ms |
| Low Male, Instrument | 50–450 / 50–1200 Hz | 20,2 ms |

### `aura::pitch::ScaleQuantizer` et `CorrectionCurve` (✅)

- `setKey`, `setScale` (Chromatic, Major, Minor naturel, Pentatonic majeure, Custom), `setReferenceA4` (415–466),
  `setHysteresisCents` (12 c par défaut : pas de battement entre deux notes à mi-chemin).
- Clavier Custom : `setNoteState (pc, Active | Bypass | Removed)`. Bypass : quand la voix est sur cette note, rien
  n'est corrigé. `copyCurrentScaleToCustom()` : le premier clic sur le clavier part de la gamme affichée.
- `quantize (midi)` -> `{ inputMidi, targetMidi, deviationCents, targetNote, bypassed }`.
- `CorrectionCurve::process (q, voiced, dt)` -> décalage en demi-tons = correction lissée + Transpose + FineTune.
  τ = RetuneSpeed / 3 (95 % en RetuneSpeed) + Humanize × S(t) × 350 ms, S(t) de 0 à 1 entre 80 et 300 ms de note
  tenue. Courbe du potentiomètre : `retuneMsFromNormalised (x) = 401^x − 1` (0,5 -> 19 ms ; 0,75 -> 89 ms).
- Trous non voisés de moins de 30 ms : correction maintenue ; au-delà, retour à 0 en 20 ms.

### `aura::pitch::LockFreeSpscFifo<T, N>` / `LockFreePitchFifo` (✅)

Tableau statique, indices 64 bits atomiques (acquire / release), `push` (audio, ne bloque jamais, trames perdues
comptées), `pop` / `drain` (UI). `PitchFrame` : `samplePosition`, `inputMidi`, `outputMidi`, `targetMidi`,
`deviationCents`, `clarity`, `levelDb`, `voiced`. 2048 trames = 3 s d'avance.

### `PSOLAShifter` (étape 2, Live)

- Marques de pitch posées sur les maxima d'énergie locale guidés par `periodSamples` ; grains Hann de 2 périodes ;
  espacement de synthèse = période / ratio ; grains lus dans le passé de l'entrée.
- Latence annoncée à l'hôte ≤ 2 ms (88 échantillons à 44,1 kHz, ×fs/44,1 k en DXD). **Contrainte physique à
  valider** : un grain causal de 2 périodes décrit une hauteur passée ; le contenu sort avec un retard d'environ
  une période (1 à 20 ms selon la note), comme sur les correcteurs matériels temps réel. Les transitoires
  (non voisé, ou clarté qui chute) passent sans regranulation.
- `prepare (fs, maxBlock)`, `setRatio (double)` lissé, `process (const T*, T*, n, const PitchEstimate&)`.

### `PhaseVocoderShifter` (étape 2, Studio/DXD)

- FFT de durée constante (≈ 46 ms : 2048 points à 44,1 kHz, 16 384 à 352,8 kHz) : même résolution en Hz dans
  les graves à toutes les fréquences, recouvrement ×4, verrouillage de phase vertical (pics, Laroche-Dolson) et
  horizontal (cohérence d'une trame à l'autre), rééchantillonnage final en sinc polyphase.
- Latence annoncée = taille de FFT ; le détecteur est retardé d'autant pour que la correction tombe sur la note.

### `FormantProcessor` (étape 2)

Enveloppe spectrale par cepstre (lifter à 0,6 × période), FormantShift −12…+12 demi-tons (déformation de
l'enveloppe), FormantPreserve : l'enveloppe d'origine est réappliquée après le décalage de hauteur.

### `AuraPitchProcessor` (étape 3)

- `supportsDoublePrecisionProcessing() = true`, `processBlock (AudioBuffer<float>&)` et `<double>&` vers un
  `processTemplated<T>` unique ; tout est préparé dans `prepareToPlay` ; `juce::ScopedNoDenormals`.
- `SmoothedValue` (multiplicatif) sur le ratio, le Mix et le FormantShift ; Key / Scale / InputType lus une fois par
  bloc depuis l'APVTS.
- Changement de TrackingMode : les deux moteurs sont préparés, fondu 10 ms, `setLatencySamples` mis à jour depuis le
  message thread (`AsyncUpdater`), Dry réaligné.
- `LockFreePitchFifo` publiée vers l'éditeur ; état en XML APVTS + clavier Custom.

### `AuraPitchEditor` (étape 3)

Fond `#0B0C10`, panneaux `#12151E`, cible cyan `#00F0FF`, voix entrante ambre/violet ; `PitchGraph` défilant
(`juce::VBlankAttachment`, 60/120 Hz, tracé sur l'horloge `samplePosition`), `CentsMeter`, `ScaleKeyboard` 12 notes
(clic : Active ↔ Removed, Alt-clic : Bypass).

## Paramètres (APVTS, identifiants figés)

| ID | Plage | Note |
|---|---|---|
| `input_type` | Soprano, Alto/Tenor, Low Male, Instrument | `PitchDetector::setInputType` |
| `key` | C … B | |
| `scale` | Chromatic, Major, Minor, Pentatonic, Custom | |
| `note_0` … `note_11` | Active, Bypass, Removed | clavier Custom |
| `retune_speed` | 0 … 400 ms | normalisé `401^x − 1` |
| `humanize` | 0 … 100 % | |
| `formant_shift` | −12 … +12 demi-tons | |
| `formant_preserve` | On / Off | |
| `transpose` | −24 … +24 demi-tons | |
| `fine_tune` | −100 … +100 cents | |
| `reference_a4` | 415 … 466 Hz | |
| `tracking_mode` | Live, Studio/DXD | change la latence |
| `mix` | 0 … 100 % | Dry aligné sur la latence |

## Banc d'essai (étape 1)

`ctest --test-dir build` ou `aurapitch_dsp_test` : **247 / 247** avec GCC 13 et Clang 18 ; AddressSanitizer + UBSan
et ThreadSanitizer sans aucun avertissement (seul le budget CPU échoue sous sanitizer, normal).

| Mesure | Résultat |
|---|---|
| Voyelle synthétique 55 … 1175 Hz, 8 fréquences de 44,1 à 384 kHz (88 cas) | erreur max **0,033 cent**, 100 % voisé |
| Vibrato ±50 c à 5,5 Hz (comparé à la F0 vraie à `frameCentre`) | RMS 1,34 c à 48 kHz, 1,35 c à 384 kHz |
| Glissando d'une octave en 1 s | RMS 0,8 c |
| Voyelle + souffle 200 Hz–12 kHz à 20 dB sous la voix | 100 % voisé, médiane 0,45 c, max 2,4 c |
| Sifflante 4–10 kHz, bruit blanc, souffle, silence | 0 % voisé |
| Dents de scie 1 kHz (synthé brillant) | 100 % voisé, max 0,04 c |
| Low Male C2 / Soprano B5 | 0,02 c / 0,05 c, aucune erreur d'octave |
| Entrée float = entrée double | < 0,01 c |
| Allocations dans `process()` et `setInputType()` | 0 |
| Charge CPU, plage 50–1200 Hz, une estimation / 1,5 ms (Xeon 2,1 GHz virtuel, un cœur) | 1,3–2,5 % de 44,1 à 384 kHz |
| Retune 0 ms / Humanize 100 % sur vibrato ±50 c | sortie exactement sur la note / vibrato conservé à ±50 c, note moyenne +0,3 c |
| Retune 50 ms | 95 % de la correction en 51 ms |
| FIFO : 2 000 000 trames concurrentes | reçues dans l'ordre, 0 allocation |

## Limites et points ouverts

- Signaux de test synthétiques : un passage sur des voix réelles (sifflantes, voix soufflées, fry) doit confirmer
  les seuils de voisement avant l'étape 3. Ils sont réglables par `PitchDetector::Config`.
- Latence d'analyse de 7 à 20 ms selon l'InputType : en Live, les premières millisecondes de chaque note sont
  corrigées avec ce retard (comportement des correcteurs temps réel) ; en Studio/DXD, l'audio est retardé d'autant.
- Plateformes : développé et testé sous Linux ; macOS (universel arm64 + x86_64) et Windows via la CI à venir.
