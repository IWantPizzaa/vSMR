# Validation sur les replays EuroScope locaux

Les fichiers originaux de `aa-REPLAYS` sont ouverts en lecture seule. Le scan parcourt **27 fichiers**, soit **2.452 Go** ; **11** contiennent des positions proches de LFPG, indépendamment du nom de fichier. Les dérivés ignorés de `tmp/replay-cache` ne conservent que la télémétrie anonyme, le type et les aéroports. Les messages d'authentification, identités personnelles et conversations ne sont ni exportés ni nécessaires aux tests.

Les **6 sessions** ci-dessous donnent **445 753 rapports de surface proches du graphe**. **224 références de parcours standard provisoires** sont rapprochées de **71 214 rapports réellement enregistrés**. Le suivi emploie les intervalles réels du journal, sans les remplacer par une cadence artificielle de cinq secondes. Les horodatages sont ceux de réception du journal, à la seconde, avec passage de minuit déroulé ; les répétitions au même instant ne comptent pas comme nouveaux votes.

**Ces replays ne contiennent pas la clairance complète de roulage attribuée ni un étiquetage des collisions.** On ne peut donc pas annoncer un taux de faux positifs ou assimiler les différences au standard à des erreurs de pilote. Le stand immobile, le point d'accès de piste proche, le plan de vol et un profil connu permettent seulement de construire une référence plausible à examiner. Les clairances de traversée inconnues ne sont pas évaluées comme des infractions ; les projections de conflit s'arrêtent au prochain nœud du segment actuellement observé, au plus à dix secondes.

**186 des 224 trajectoires comparées présentent au moins une différence persistante avec la référence standard inférée.** Ce résultat non nul est conservé : il signale des différences de cheminement, d'ancrage supposé ou de données historiques à examiner, et ne prouve pas une faute du pilote. 31 rapports classés dans un état d'écart confirmé restent géométriquement à moins de10m du tracé de référence, ce qui mérite aussi une revue des raccordements proches. 5 rapports sont rejetés par les garde-fous de suivi.

| Session | Rapports surface proches du graphe | Références provisoires | Rapports comparés | Paires de proximité à examiner |
| --- | ---: | ---: | ---: | ---: |
| PBN_08_05_26.txt | 34943 | 21 | 6144 | 0 |
| PBN_09_04_26.txt | 76533 | 31 | 8630 | 2 |
| PBN_20_03_26.txt | 67905 | 46 | 12976 | 1 |
| PBN_21_02_26.txt | 80593 | 54 | 13661 | 2 |
| TGL_SightSeeing_LFPG_22_03_26.txt | 61464 | 37 | 16131 | 5 |
| WF2025.txt | 124315 | 35 | 13672 | 3 |

| Session | Intervalle médian entre rapports retenus | 95e percentile |
| --- | ---: | ---: |
| PBN_08_05_26.txt | 5 s | 6 s |
| PBN_09_04_26.txt | 5 s | 6 s |
| PBN_20_03_26.txt | 5 s | 6 s |
| PBN_21_02_26.txt | 5 s | 6 s |
| TGL_SightSeeing_LFPG_22_03_26.txt | 5 s | 6 s |
| WF2025.txt | 5 s | 6 s |

Les intervalles ci-dessus viennent des rapports de la même génération de cible, dans la sélection géographique/altitude/vitesse. Un grand intervalle peut aussi refléter une sortie de cette sélection ; ce n'est pas à lui seul une preuve de perte réseau.

Le bit FSD sol n'est pas renseigné dans la majorité des rapports. Le filtre retient une altitude entre200 et600 pieds, une vitesse de40kt maximum, une distance de45m maximum aux voies/positions de stationnement, puis des trajectoires durables avec mouvement. Il ne prétend pas transformer ces critères en capteur sol/vol. Les enregistrements2025/2026 sont comparés au graphe et aux règles actuellement livrés : les restrictions de travaux sont exclues, mais les différences historiques restent possibles.

Les champs `@`, l'altitude en pieds, la vitesse en nœuds et le PBH sont vérifiés contre les sources primaires [Swift PilotDataUpdate](https://apidocs.swift-project.org/pilotdataupdate_8cpp_source.html) et [Swift PBH](https://apidocs.swift-project.org/pbh_8h_source.html). Le cap PBH est décodé comme bits2–11 multipliés par360/1024 ; sa référence vrai/magnétique n'étant pas déclarée par le journal, le tracker exploite le déplacement géométrique pour sa direction vraie.

Le contrôle des proximités utilise les nouvelles enveloppes physiques, avec longueur, envergure et orientation. L'axe du corps est une **estimation** obtenue après au moins12m de déplacement géométrique, puis mise à jour par les variations relatives du PBH ; ce n'est pas un cap vrai de fuselage vérifié. Les premières positions stationnaires sans axe établi sont exclues de cette comparaison. La direction du nez pendant un repoussage reste une limite de cet oracle. Les résultats sont des candidats à examiner et ne prouvent aucun impact réel.

40005 instantanés actifs possèdent un axe estimé pour cette comparaison physique ; 31619 sont exclus faute d'orientation établie. Il s'agit de comptages par cycle de prévision, pas d'autant de rapports indépendants supplémentaires.

## Reproduction

`python scripts/import_euroscope_replays.py` lit le répertoire local fourni, puis `npm.cmd run validate:replays` rejoue les caches. `python tests/test_euroscope_replay.py` vérifie le format, le passage de minuit, le PBH, les unités et le filtrage des données non télémétriques. Résultats, intervalles, exclusions, identifiants anonymes et empreintes des fichiers : [replay-validation-results.json](replay-validation-results.json). Les originaux ne sont pas inclus dans le projet.
