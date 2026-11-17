# AuraPitch — Lot 1 (cœur DSP) et Lot 2 (interface, Mode Ultra, export hors ligne)

Base de comparaison : commit `0ea8279` (étape 6, « avant »). Toutes les mesures sont faites sur le même banc
(`Tests/Lot1Bench.cpp`), compilé contre l'ancien moteur (`-DAURAPITCH_LOT1=0`) et contre le nouveau.

## Lot 1 — résultats avant / après

### 1. Transparence (Preserve OFF, Retune 0 : la sortie doit être l'entrée transposée)

Voix stable A3, harmoniques jusqu'à 20 kHz, chaque harmonique de sortie (≤ 19,5 kHz) comparée à l'harmonique
d'entrée de même rang.

| Cas | Avant : énergie / raie à la fréquence nominale / hauteur | Après |
|---|---|---|
| 44,1 kHz, +12 dt | −0,58 dB / −0,80 dB / +0,016 c | +0,001 dB / −0,01 dB / −0,003 c |
| 48 kHz, +19 dt | −0,39 dB / −1,52 dB / +0,035 c | +0,000 dB / −0,02 dB / −0,005 c |
| 48 kHz, −12 dt | +0,004 dB / −1,05 dB / −0,066 c | −0,004 dB / −0,01 dB / −0,005 c |
| Studio, 384 kHz, +12 dt | −0,033 dB / −0,63 dB / +0,025 c | +0,000 dB / −0,02 dB / −0,005 c |
| **Vérifications du banc** | **79 / 128** | **128 / 128** |

Tête de lecture seule, sinusoïde transposée, niveau de la raie de sortie :

| | 19 kHz | 19,5 kHz | 20 kHz |
|---|---|---|---|
| Avant, 44,1 kHz +12 dt | −0,33 dB | −0,69 dB | — |
| Avant, 48 kHz +12 dt | −0,00 dB | −0,03 dB | −0,12 dB |
| Après (tous les cas, 44,1 / 48 kHz, +2 … +12 dt) | ±0,02 dB | ±0,02 dB | ±0,02 dB |

Repliement (image transposée au-delà de fs / 2) : +12 dt à 48 kHz −67 -> **−92 dB** (Ultra −130 dB) ; +19 dt
−28 -> **−128 dB**.

Les « 2,4 dB à +5 dt, 48 kHz » notés à l'étape 5B ne se reproduisent pas sur ce banc : avant le lot 1, la pire
perte mesurée par harmonique est de 0,58 dB. Une large part des pertes « à 19 kHz » d'une raie fine venait en
réalité d'une **dérive de hauteur de quelques centièmes de cent** (le raccord était placé à ≈ 10⁻² échantillon de
la période) : à 19 kHz, un écart de 0,007 c suffit à faire perdre 0,05 dB à une raie mesurée dans une fenêtre de
1,5 s.

Corrections :
- `Interpolator.h` : pour une lecture accélérée (r > 1), le noyau sinc est **étiré** d'un facteur r
  (48·r points) au lieu d'être rogné à 48 points avec une coupure abaissée. La coupure reste à fs / 2 vue de la
  sortie, et la transition garde sa largeur : la réponse est la même qu'à r = 1.
- `SpliceShifter.h` : le sommet de la corrélation du raccord est affiné à retard fractionnaire (parabole à 0,05
  puis 0,01 échantillon). Le saut tombe sur la période à ≈ 10⁻⁴ échantillon, et la hauteur de sortie ne dérive plus.
- Mode Ultra : noyau de 96 points, Kaiser β = 12,5 (≈ 125 dB).

### 2. Résidu Live sur vibrato fort (Retune 0, 5,8 Hz, voix réelle), crête / RMS en cents

| | ±30 c | ±60 c | ±100 c |
|---|---|---|---|
| Live A3, avant | 14,7 / 7,6 | 29,6 / 16,4 | 198,8 / 66,0 (décroche) |
| Live A3, après | **2,9 / 0,7** | **5,0 / 1,0** | **5,9 / 1,2** |
| Live A2, avant -> après | 18,7 / 9,8 -> 3,3 / 0,6 | 37,0 / 19,5 -> 5,1 / 1,1 | 179 / 45 -> 5,8 / 1,4 |
| Live A3 384 kHz, avant -> après | 14,3 / 7,9 -> 4,8 / 0,9 | 29,4 / 16,9 -> 5,0 / 1,1 | 197 / 67 -> 12,0 / 1,5 |
| Studio A3, avant -> après | 5,6 / 1,5 -> 1,2 / 0,3 | 9,5 / 2,7 -> 3,4 / 0,4 | 204 / 63 -> 5,5 / 0,8 |
| **Vérifications** | **11 / 56 avant** | **56 / 56 après** | |

Plancher de mesure (même voix sans vibrato) : 2,7 / 0,7 c en Live.

Corrections :
- `PitchDetector.h` : **suivi rapide**. Une fois la note trouvée par la fenêtre longue, la période est ré-estimée
  sur 2,5 périodes de la note chantée, dans ±3 % de l'estimation précédente.
- `FastPitchTrack.h` (nouveau) : historique daté. La hauteur est interpolée à l'instant que la tête lit vraiment,
  ou extrapolée de quelques ms par une parabole des moindres carrés sur 24 ms.
- `AuraPitchEngine.h` : en Live, traitement par tranches d'un pas d'analyse (1,5 ms) et rapport recalé toutes les
  ≈ 0,7 ms. La fenêtre longue garde la décision (note, voisement, octave).
- `ScaleQuantizer.h` : la note est tenue sous un vibrato large (±100 c ne fait plus sauter de note).

### 3. Latence

| | Avant | Après |
|---|---|---|
| Latence annoncée Live (tous fs, 44,1 … 384 kHz) | 2,0 ms | 2,0 ms |
| Latence annoncée Studio DXD | 55,7 … 61,5 ms | inchangée |
| Retard de la hauteur utilisée en Live, A3 (220 Hz) | 13,5 ms | **5,7 ms** |
| idem, A4 (440 Hz) / A5 (880 Hz) | 13,5 / 13,5 ms | **2,9 / 1,4 ms** |
| idem, voix grave (110 Hz) | 20,2 ms | **11,4 ms** |
| idem, DXD 384 kHz, A3 / A4 | 13,9 ms | **6,1 / 3,3 ms** |
| Attaque A4 +40 c : dernière trame à plus de 5 c de la note (Live) | 10,6 ms | 10,6 ms (inchangé) |

Le retard est maintenant de 1,25 période de la note chantée, au lieu de 2 périodes de la note la plus grave du
registre. La précision est conservée : l'erreur maximale de la hauteur rapide est de 0,06 à 0,49 c, et une voix
immobile garde la fenêtre longue (±0,04 c).

L'attaque ne change pas. Elle dépend de la décision de voisement de la fenêtre longue, et la décider sur 2,5
périodes ferait courir un risque d'erreurs d'octave sur les attaques. Ce changement est à valider sur voix réelle.

### 4. Charge CPU (stéréo, voix corrigée en continu, % d'un cœur, `-O3`)

| | Avant | Après |
|---|---|---|
| Live 48 kHz | 2,0 % | 2,0 % |
| Live 352,8 kHz | 8,8 % | 8,4 – 9,0 % |
| Live 352,8 kHz, Formant +2 (PSOLA) | 10,2 % | 13,3 % |
| Live 384 kHz, Formant +2 (PSOLA) | 11,2 % | 14,0 % |
| Studio 384 kHz (banc du lot 1) | 10,2 % | 10,0 % |
| Allocations dans `process()` | 0 | 0 |

Le surcoût en PSOLA vient du noyau étiré : 54 points au lieu de 48 à Formant +2, et environ 19 instructions par
point au lieu de 10, car le pas dans la table n'est plus entier. C'est le prix de la réponse plate à 20 kHz quand
les grains sont lus en accéléré. Un clamp à chaque point coûtait encore +40 % ; il est sorti de la boucle.

## Lot 2 — interface et export

- **Vue MONITOR** :
  - **AURA CORE** remplace l'accordeur. Couronne des 12 notes : gamme éclairée, note visée lumineuse, tonique
    marquée. Deux repères tournent sur la couronne, la voix entrante (ambre) et la voix corrigée (cyan), reliés par
    l'arc de correction. Halo de particules qui respire avec le niveau et s'agite avec l'écart. Au centre : note,
    écart d'entrée, LOCKED (±3 c) et fréquence.
  - Historique de hauteur à côté.
- **Colonne CORRECTION**, toujours visible :
  - RETUNE et MIX en grand ;
  - jauge **PULL** (correction appliquée en ce moment, ±50 c) ;
  - **NOTE HOLD** : OFF · SHORT · MID · LONG règle Flex sur 0 / 40 / 90 / 160 ms en un clic. Le segment s'éteint si
    Flex est réglé à une autre valeur.
  - Liseré cyan en Live, violet en Studio DXD.
- **Réglages fins** dans la bande du bas : Flex, Humanize | Formant, Preserve, Throat | Transpose, Fine Tune,
  A4 Ref | Doubler.
- **Workflow** : grand sélecteur LIVE / STUDIO DXD qui affiche la latence réelle de chaque mode.
- **Onglets** MONITOR | GRAPH | SETTINGS, avec le bouton ULTRA dans la barre.
- **SETTINGS** :
  - interrupteur ULTRA MODE ;
  - état du moteur : STANDARD, ULTRA, ou ULTRA · OFFLINE BOUNCE ;
  - rappel des deux workflows ;
  - taille de l'interface (75 / 100 / 125 / 150 / 200 %) ;
  - session (fréquence, précision, latence).
- **Mise à l'échelle** : l'interface est dessinée à 960 × 640 puis agrandie d'un seul facteur, de 75 à 250 %,
  proportions verrouillées. On la règle au coin inférieur droit ou dans SETTINGS. Textes, traits et zones cliquables
  suivent, et l'échelle est sauvegardée avec le projet.
- **Mode Ultra et export hors ligne** (`PluginProcessor.cpp`, `processTemplated`) :
  - à chaque bloc, `isNonRealtime()` force `ultra` dans la copie des paramètres envoyée au moteur. Le paramètre
    `ultra_mode` de l'utilisateur n'est jamais modifié : à la fin de l'export, le moteur reprend tout seul son réglage ;
  - la latence est la même dans les deux qualités, donc rien à annoncer à l'hôte et l'export reste aligné ;
  - `ultra_mode` n'est pas automatisable, et les presets d'usine n'y touchent pas ;
  - au banc, en Live comme en Studio : Ultra engagé pendant le bounce, réglage intact, retour à l'état initial,
    latence identique, aucun clic aux deux bascules (énergie > 6 kHz égale), et rendu identique **au bit près** au
    mode Ultra activé à la main.

## Bancs

| Banc | Résultat |
|---|---|
| `aurapitch_lot1_test` (nouveau) | 378 / 378 |
| `aurapitch_plugin_test` | 102 / 102 (dont export hors ligne, SETTINGS, échelle, AURA CORE, NOTE HOLD) |
| `aurapitch_dsp_test` · `engine_test` · `realvoice_test` · `step5a_test` · `step5b_test` · `aurakey_test` | 247 · 134 · 32 · 209 · 83 · 159, tous passent |

## Limites

- Signaux de synthèse réalistes (jitter, shimmer, souffle, vibrato), mais pas d'acapella réelle au banc.
- Attaque Live inchangée (10,6 ms) : voir la section Latence.
- Interface rendue et testée sous Linux (Xvfb) ; à vérifier sur macOS (Retina, M5), où le rendu est vectoriel et
  mis à l'échelle de la même façon.
