# AuraPitch 1.0 — NextDSP · architecture (étapes 1 à 4)

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
- Étape 4 : refonte du cœur après l'écoute sur acapella réelle (dédoublement, voile 2–12 kHz, timbre métallique) :
  `SpliceShifter` (une seule tête de lecture, raccords glottiques), interpolation sinc 48 points, filtre de formants
  différentiel en Studio, voisement maintenu 50 ms. Banc `aurapitch_realvoice_test` **32 / 32** ; banc de l'étape 2
  adapté : **134 / 134**. Latence Studio ramenée de ≈ 110 ms à ≈ 56–62 ms.
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
│   │   ├── SpliceShifter.h            ✅ étape 4 : tête de lecture unique, raccords synchrones de la période (T* sous-échantillon)
│   │   ├── Interpolator.h             ✅ étape 4 : sinc fenêtré Kaiser 48 points, 512 phases, 29 bancs anti-repliement
│   │   ├── PSOLAShifter.h             ✅ TD-PSOLA (effets) : marques par corrélation locale, grains sinc, fondu V/UV
│   │   ├── StudioFormantFilter.h      ✅ étape 4 : Studio/DXD = découpe « delta » + raccords + filtre de formants différentiel
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
│   ├── RealVoiceSelfTest.cpp          ✅ banc étape 4 (voix réaliste : jitter, shimmer, souffle, consonnes, flow rapide)
│   ├── VocalRealism.h                 générateur de voix réaliste, rejet de voix fantôme, écarts par tiers d'octave
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
`formantPreserve`, `transpose`, `fineTuneCents`, `referenceA4`, `mode` (Live / Studio), `mix`. Mix est lissé sur
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
| Live | 3,7 % | 4,0 % | 5,4 % | 11,1 % | 16,7 % | 18,5 % |
| Studio/DXD | 3,8 % | 3,8 % | 3,6 % | 4,6 % | 5,8 % | 6,6 % |

Étape 4 : le banc applique Formant +2 dt, donc en Live les deux cœurs tournent (PSOLA rendu, raccords prêts pour le
fondu) à la fréquence native ; la sinc 48 points coûte plus que l'Hermite. Studio travaille à 44,1/48 kHz quelle que
soit la fréquence et devient moins cher qu'avec le vocodeur.

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

### Banc `aurapitch_plugin_test` (55 vérifications)

| Mesure | Résultat |
|---|---|
| Paramètres, textes avec unités, courbe Retune | 26 paramètres · « 0.0 ms » / « 19 ms » / « 400 ms », « +3.00 st », « 440.0 Hz » |
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
| Flow rapide, 8 syllabes, glissés 25 ms, consonnes, vibrato | Studio 100 % des trames à ±8 c (max 4,3 c) · Live : 7 syllabes à ±7,2 c, la syllabe en vibrato à 16,9 c |
| Stéréo Linked, voix centrée | \|L − 0,7 R\| = 1·10⁻¹⁶ : mêmes marques, même phase |

**Limite Live** : le détecteur a 13,5 ms de retard ; un vibrato de ±30 c aplati à Retune 0 laisse un résidu de cet
ordre (17 c). Studio, qui lit la commande dans le futur, n'a pas ce résidu.
