# AuraPitch — NextDSP · AuraSuite

Correction de hauteur et Pitch & Formant Shifter **VST3 / AU / Standalone**, de 44,1 kHz au **DXD 352,8 / 384 kHz**,
traitement 64 bits. C++20 · JUCE 8.0.15.

![AuraPitch](docs/screenshot.png)

- **Live** (2 ms) : une seule tête de lecture à raccords glottiques (interpolation sinc 48 points), TD-PSOLA à
  marques par corrélation pour les effets de formants.
- **Studio/DXD** (≈ 56–62 ms, constante) : même cœur à 44,1/48 kHz, filtre de formants différentiel (module seul,
  enveloppe vraie), architecture « delta » qui laisse intact tout ce qui dépasse la bande vocale (air du DXD).
- Sur voix réaliste : rejet de la voix fantôme 83–87 dB sur voix stable, transmission ±0,05 dB de 100 Hz à
  19,5 kHz, stéréo liée en phase exacte.
- Détection MPM 50–1200 Hz à coût constant quelle que soit la fréquence, voisement anti-sifflantes.
- Gammes, clavier 12 notes (Active / Bypass / Removed), Retune 0–400 ms, Humanize, Transpose ±24, Fine ±100 c,
  A4 415–466 Hz, Formant ±12, Mix aligné à l'échantillon, stéréo liée ou double mono, bypass sans clic.
- **Expressif (étape 5A)** :
  - **Flex** (Transition 0–200 ms) : glissandos et slides d'attaque passent tels quels, puis la note se verrouille ;
  - **Throat** 80–120 % : F1–F3 déplacés comme par un conduit vocal plus long ou plus court, aigus intacts ;
  - **Doubler** 0–100 % : deux voix à +9 ms / −7 c et +14 ms / +7 c (stéréo) ;
  - **AUTO-KEY** : détection de la tonalité (Temperley, bayésien) et application en un clic ;
  - **MIDI Target** : la voix va à la note MIDI jouée (la plus proche dans un accord), Pitch Bend ±2 dt.
- Ergonomie : glissé fin ÷5 (Maj ou Cmd/Ctrl), double-clic = valeur par défaut, clic sur la valeur ou clic droit =
  saisie au clavier, **FREEZE** du graphe, notes MIDI éclairées sur le clavier et la grille.

![AuraPitch, MIDI Target et Auto-Key](docs/screenshot-5a.png)

- **GRAPH EDITOR (étape 5B)**, sans audio en RAM : **CAPTURE** enregistre les notes chantées sur la timeline du DAW
  (AudioPlayHead, ≈ 50 Ko par minute de chant), segmentées automatiquement en notes (liaisons legato comprises) ;
  chaque note se déplace (demi-ton aimanté ou au cent), se quantifie (double-clic), se scinde (clic droit), perd ou
  gagne son vibrato (Cmd/Ctrl), reçoit un glissé d'attaque (poignée gauche) et son propre formant (Cmd/Ctrl + Alt).
  Au rejeu, la correction est calculée d'après la hauteur capturée, pour l'instant exact lu, sans retard du
  détecteur, en Live comme en Studio. Les notes sont sauvegardées avec la session.

![AuraPitch, GRAPH EDITOR](docs/screenshot-graph.png)

## Compiler

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release        # JUCE 8.0.15 est téléchargé depuis GitHub
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

| Système | Pré-requis | Produits (`build/AuraPitch_artefacts/Release/`) |
|---|---|---|
| macOS 11+ (Apple Silicon et Intel) | Xcode 15 + CMake ≥ 3.22. Binaire universel par défaut | `VST3/AuraPitch.vst3`, `AU/AuraPitch.component`, `Standalone/AuraPitch.app` |
| Windows 10+ | Visual Studio 2022 (C++), CMake ≥ 3.22 | `VST3/AuraPitch.vst3`, `Standalone/AuraPitch.exe` |
| Linux | `libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libxcomposite-dev libfreetype-dev libfontconfig1-dev` | `VST3/AuraPitch.vst3`, `Standalone/AuraPitch` |

Options : `-DJUCE_DIR=/chemin/JUCE` (JUCE local), `-DAURAPITCH_COPY_PLUGIN=ON` (copie dans
`~/Library/Audio/Plug-Ins/VST3` et `Components` après compilation), `-DAURAPITCH_BUILD_PLUGIN=OFF` (noyau DSP et
bancs seuls, sans JUCE). La cible `AuraPitchTests` construit les six bancs.

**AU** : depuis l'étape 5A, l'AU est un *Music Effect* (`aumf`, effet audio avec entrée MIDI) pour recevoir les
notes du mode MIDI Target ; dans Logic, il s'insère comme effet et la piste MIDI se route vers lui (*side chain*
MIDI). Une session qui contenait l'AU de type `aufx` des étapes précédentes doit le réinsérer.

**macOS, première ouverture** : le plug-in n'est pas signé par un compte développeur Apple (signature ad hoc). Si
le DAW le refuse : `xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/AuraPitch.component`
(idem pour le `.vst3`).

**CI** : `.github/workflows/build.yml` compile macOS (universel), Windows x64 et Linux, lance `ctest`, valide l'AU
avec `auval` et publie les archives ; une étiquette `v*` crée une release GitHub.

## Bancs d'essai

| Cible | Contenu | Résultat |
|---|---|---|
| `aurapitch_dsp_test` | détecteur, gamme, courbe de correction, FIFO | 247 / 247 |
| `aurapitch_engine_test` | FFT, enveloppe, PSOLA, raccords, Studio/DXD, moteur, temps réel | 134 / 134 |
| `aurapitch_realvoice_test` | voix réaliste : interpolation, voix fantôme, voile, flow rapide, stéréo | 32 / 32 |
| `aurapitch_step5a_test` | Auto-Key, MIDI Target, Flex, Throat, Doubler, transparence au bit près, CPU, temps réel | 209 / 209 |
| `aurapitch_step5b_test` | GRAPH EDITOR : capture et segmentation, édition et rejeu, transparence, scission et état, temps réel avec éditions concurrentes, processeur (AudioPlayHead, état), interface | 66 / 66 (48 sans JUCE) |
| `aurapitch_plugin_test` | processeur JUCE : paramètres, presets, latence, 32/64 bits, double mono, bypass, état, éditeur, MIDI de l'hôte, ergonomie 5A | 73 / 73 |

Architecture détaillée et mesures : [ARCHITECTURE.md](ARCHITECTURE.md).

## Licences

JUCE 8 : AGPLv3 ou licence commerciale JUCE. Distribuer un binaire d'AuraPitch impose l'une des deux.
