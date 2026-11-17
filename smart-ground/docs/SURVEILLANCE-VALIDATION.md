# Validation des observations sol espacees

Commande : `npm.cmd run validate:surveillance`. Les 16 parcours LFPG couvrent les deux configurations, des arrivees et departs, A320, A388, B744 et B748. Le suivi recoit seulement position x/y, cap vrai, vitesse sol et horodatage independant toutes les cinq secondes ; les identifiants de segment du simulateur ne lui sont pas transmis.

Les **1679 observations bruitees** (position ±3 m par axe, cap ±3°, vitesse ±0,25 kt) ne produisent **aucun faux ecart confirme**. 31901 estimations intermediaires ne creent ni nouvel horodatage source ni vote de confirmation. Les intersections et voies paralleles peuvent rester ambigues ; un ecart isole n'est pas transforme en certitude. Les 2 traversees necessaires au parcours complet sont autorisees explicitement par l'oracle de test ; le suivi ne delivre aucune clairance.

Une mauvaise branche U reelle depuis M09 vers T3 est confirmee apres 15 secondes, sur au moins deux rapports independants et non par repetition des images. La vitesse de2m/s de cet oracle rend la branche durablement observable. L'extrapolation est bornee a dix secondes, puis figee et signalee perimee ; apres vingt secondes le suivi est perdu. Une projection ne franchit pas un point d'attente non autorise, mais une mesure effectivement recue au-dela reste disponible comme preuve de deviation.

Un detour commis puis corrige entierement entre deux messages n'est pas observable avec ces seules donnees. La cadence de cinq secondes est ici un scenario de simulation, pas une promesse SDK. Quand EuroScope ne donne pas l'heure de mesure, l'adaptateur doit employer l'epoque de reception independante et conserver l'age connu ; chaque image de rendu n'est pas une nouvelle observation. Le bruit et les seuils sont des parametres a calibrer sur des enregistrements reels.

| Profil | Configuration | Origine → destination | Rapports | Ambigus | Suspects isoles | Faux confirmes |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| A320 | east | stand:B08 → operation:departure:09R:D3 | 124 | 17 | 0 | 0 |
| A320 | east | stand:M09 → operation:departure:08L:T3 | 39 | 7 | 0 | 0 |
| A320 | east | stand:B03 → operation:departure:09R:D5 | 112 | 15 | 1 | 0 |
| A320 | east | stand:A01 → operation:departure:08L:T4 | 36 | 6 | 0 | 0 |
| A320 | west | stand:B12 → operation:departure:26R:T9 | 116 | 14 | 0 | 0 |
| A320 | west | stand:F02 → operation:departure:26R:T11 | 92 | 20 | 0 | 0 |
| A320 | west | operation:arrival:27R:K3 → stand:I24 | 78 | 12 | 0 | 0 |
| A320 | east | operation:arrival:09L:K6 → stand:F02 | 113 | 15 | 0 | 0 |
| A388 | east | stand:K09 → operation:departure:08L:T4 | 122 | 20 | 0 | 0 |
| A388 | east | stand:W6/W7 → operation:departure:09R:D4 | 50 | 11 | 1 | 0 |
| A388 | west | stand:Y08 → operation:departure:26R:T11 | 166 | 34 | 0 | 0 |
| A388 | west | stand:W6/W7 → operation:departure:27L:Q5 | 66 | 8 | 0 | 0 |
| B744 | east | stand:K09 → operation:departure:08L:T4 | 111 | 10 | 0 | 0 |
| B744 | west | stand:Y08 → operation:departure:26R:T11 | 166 | 36 | 0 | 0 |
| B748 | east | stand:K09 → operation:departure:08L:T4 | 122 | 20 | 0 | 0 |
| B748 | west | stand:Y08 → operation:departure:26R:T11 | 166 | 36 | 0 | 0 |

Resultats detaillees : [surveillance-validation-results.json](surveillance-validation-results.json). Les tests unitaires couvrent aussi les doublons, paquets reordonnes, cap d'un avion arrete, repoussage, virages normaux entre deux rapports et saut de position incoherent.

## Empreintes

- `public/data/lfpg.json` : `4597cfcb70d555ca801427a378d7f8fca297e55677d3062faed0c1718ed5b9c5`
- `public/data/lfpg-operations.json` : `d881f3494335470dfa11a0fe371f99bc7f2a772c2bca7e7a1785f74d7b603736`
- `public/data/lfpg-corridors.json` : `9228e0c4e9293426c2318ea032ce5ac07fc331595201b8c6eeaa6916cdf8def3`
- `public/data/lfpg-aircraft-restrictions.json` : `913a066d2bfde160f7fdf716942c58a8ac5530931a033d74f0033fae8ec63882`
- `src/core/SurfaceTracker.ts` : `beb2e482e3e56ae25462836eeb1305966ec502cf6a2d3fe0ac23cb3ffc3df7d4`
- `src/core/TaxiGraph.ts` : `d525d65afb29d17aa71eb5e68759026ab9d987e17cd7f447136c02666306db94`
- `src/core/AircraftSimulator.ts` : `8a0bc976b51e5dcb42c94eef8897da0e5add3204bab02af7bd5a320df89b23de`
- `src/core/RoutePlanner.ts` : `e7061497e46a5ba70a084019364ab5fac2b6976452bffbfed91bc57295b1c845`
- `src/core/OperatingRules.ts` : `70f833e727ff7fb0e0b0a7ce521e842f0179040ac5d3bf69b5572183cd67c92a`
- `src/core/AircraftRestrictions.ts` : `7c16fc652aa15408b1cb3ffc265ff9a9fcd5ad9cfb1a3856c10e083ea1865fe7`
- `src/core/TaxiFlowPolicy.ts` : `6ac10f84635f10d92dab1415354b3c5668047194687318f3b52d0931ea14c508`
- `scripts/validate-surveillance.ts` : `e6df21189647e0d5ff1671b35e153f56df4abdd77f086bedcd9f836f2925ec29`
