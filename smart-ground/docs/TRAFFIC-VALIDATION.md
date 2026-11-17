# Validation du trafic mixte et des mises en mouvement

Commande : `npm.cmd run validate:traffic`. Les huit scénarios utilisent le générateur mixte de l’interface, les quatre profils A320/A388/B744/B748 et les deux configurations. Chacun produit 100 itinéraires compatibles : 50 arrivées et 50 départs, avec des indicatifs uniques et au plus deux avions actifs initialement.

Les 800 itinéraires passent le rejeu complet des règles, des cheminements et des restrictions aéronefs. Les origines physiques réutilisées sont espacées d’au moins 90 secondes dans la programmation. Les avions futurs restent absents des prédictions et des conflits actifs jusqu’à leur activation. Les 96 000 positions contrôlées suivent exactement leur segment, sans erreur de cheminement injectée ni fausse alerte hors itinéraire. Aucune clairance de traversée n’est accordée.

Le contrôle de progression couvre les 30 premières secondes simulées de chaque scénario (120 pas de 0.25 s), et vérifie 960 ensembles de prédictions. Il ne prétend pas dérouler les vingt minutes du programme ni résoudre tous les conflits. Les tests d’intégration séparés couvrent les grands pas de temps, les origines occupées, le maintien de l’écart réel de 90 secondes après un retard et les autorisations de piste refusées avant activation. Le rythme de génération est un paramètre de simulation, pas une séparation ATC.

| Profil | Configuration | Arrivées / départs | Actifs au début | Programmés | Actifs après 30 s | Origines physiques | Horaires distincts |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| A320 · 35.8 m | Face est | 50 / 50 | 2 | 98 | 4 | 56 | 99 |
| A320 · 35.8 m | Face ouest | 50 / 50 | 2 | 98 | 4 | 54 | 99 |
| A388 · 79.75 m | Face est | 50 / 50 | 2 | 98 | 4 | 56 | 99 |
| A388 · 79.75 m | Face ouest | 50 / 50 | 2 | 98 | 4 | 54 | 99 |
| B744 · 64.92 m | Face est | 50 / 50 | 2 | 98 | 4 | 56 | 99 |
| B744 · 64.92 m | Face ouest | 50 / 50 | 2 | 98 | 4 | 54 | 99 |
| B748 · 68.4 m | Face est | 50 / 50 | 2 | 98 | 4 | 56 | 99 |
| B748 · 68.4 m | Face ouest | 50 / 50 | 2 | 98 | 4 | 54 | 99 |

Graine : 20261113. Données et code sont figés durant la validation ; leurs empreintes sont conservées dans [traffic-validation-results.json](traffic-validation-results.json). Les quatre catalogues sont inchangés. Les travaux restent exclus.

- `public/data/lfpg.json` : `4597cfcb70d555ca801427a378d7f8fca297e55677d3062faed0c1718ed5b9c5`
- `public/data/lfpg-operations.json` : `d881f3494335470dfa11a0fe371f99bc7f2a772c2bca7e7a1785f74d7b603736`
- `public/data/lfpg-corridors.json` : `9228e0c4e9293426c2318ea032ce5ac07fc331595201b8c6eeaa6916cdf8def3`
- `public/data/lfpg-aircraft-restrictions.json` : `913a066d2bfde160f7fdf716942c58a8ac5530931a033d74f0033fae8ec63882`
