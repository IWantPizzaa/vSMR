# Validation du prototype LFPG

Ce rapport décrit la validation géométrique de référence et les premiers contrôles du simulateur. Les règles de circulation ajoutées ensuite, les configurations est/ouest et les tests opérationnels actuels sont détaillés dans [OPERATIONS-VALIDATION.md](OPERATIONS-VALIDATION.md).

Exécution entièrement hors ligne depuis `smart-ground/` :

```powershell
npm.cmd test
npm.cmd run validate
npm.cmd run build
```

`npm run validate` régénère [validation-results.json](validation-results.json). Le rapport contient le SHA-256 exact de `public/data/lfpg.json`, les 64 itinéraires comparés, les composantes et les mesures des quatre scénarios. Les mesures de durée dépendent de la machine ; les itinéraires et les scénarios utilisent des données épinglées et la graine `20261112`.

## Réseau et géométrie

Résultat avec les données importées le 12 novembre 2026 : **7 650 nœuds, 8 487 arêtes, 11 composantes**, dont 7 601 nœuds dans la composante principale. `TaxiGraph` trie ses composantes par taille : l'identifiant `0` représente toujours la composante principale. Il conserve les nœuds et les petits réseaux isolés.

Le jeu de données contient 523 stands nommés, 451 dans le réseau principal et 72 indisponibles pour le roulage dans ce réseau. Ce dernier nombre comprend les stands sans liaison prouvée et ceux de composantes isolées. Les 8 destinations de piste sont des intersections existantes entre taxiway et piste. Le centre des pistes reste visible, mais A* exclut toutes les arêtes de type `runway` ; les avions s'arrêtent au point d'accès choisi.

Contrôles automatiques effectués :

- Chaque arête possède deux extrémités existantes, une longueur positive et un poids cohérent avec la géométrie à moins de 1 mm.
- 98 675 paires candidates ont été examinées par une grille spatiale indépendante de l'importeur : **aucun croisement géométrique de même niveau sans nœud commun ne subsiste**. Le contrôle conserve et rapporte les croisements sur des niveaux OSM différents au lieu de les connecter. L'importeur a scindé 88 intersections réelles des segments sources.
- Les 74 polygones de bâtiments AVISO ont été vérifiés avec une implémentation TypeScript indépendante, sans Shapely : 401 paires segment/bâtiment candidates, aucun segment routable pénétrant significativement dans un bâtiment.
- Le contrôle bâtiment découpe les segments aux limites des polygones, puis échantillonne les intervalles intérieurs à un pas maximal de 25 cm. Il signale plus de 1,25 m de trajet à plus de 1 m de la limite d'un bâtiment ; les très petites pénétrations sont sous sa résolution. L'importeur effectue aussi son propre contrôle géométrique exact avec un retrait de 1 m, et a exclu deux segments sources problématiques. Ces exclusions restent documentées et visibles comme géométrie de référence.
- Aucun connecteur de proximité n'est créé pour améliorer artificiellement la connectivité. Les géométries AVISO servant à l'affichage ne créent pas d'arêtes de roulage.

## Routage et mouvement

A* a été comparé à une implémentation Dijkstra indépendante pour **64 trajets** : les stands A01, C03, E16, F09, I24, N41, R 11 et Y06/Y07 vers chacun des huit accès de piste. Tous possèdent un chemin et les 64 distances correspondent à Dijkstra, sans arête de piste. Huit autres itinéraires calculés et examinés indépendamment dans `review-routes.json` ont aussi la même longueur. Le validateur vérifie l'ordre géographique ouest/est de chaque paire de pistes pour éviter de confondre les noms avec l'ordre des points d'un chemin OSM. Exemples vers 08L : A01 → 1 399,819 m ; I24 → 6 484,388 m ; Y06/Y07 → 3 426,644 m. Le rapport JSON contient la matrice complète.

Les scénarios 10, 25, 50 et 100 avions ont chacun parcouru 150 secondes simulées, par pas de 0,25 s. À chaque pas, chaque position a été comparée à l'interpolation exacte de l'arête courante, soit **111 000 positions vérifiées**, sans sortie du graphe. Le déplacement consomme les distances cumulées de l'itinéraire : un grand pas de temps conserve la progression sur les segments et se termine exactement à destination. Les avions arrivés restent présents comme obstacles.

Mesures d'une exécution locale du validateur, en millisecondes :

| Avions | Génération des routes | Prédiction moyenne, horizon 120 s | Prédiction maximale | Positions contrôlées |
| ---: | ---: | ---: | ---: | ---: |
| 10 | 8,336 | 0,187 | 0,841 | 6 000 |
| 25 | 18,004 | 0,353 | 0,763 | 15 000 |
| 50 | 33,874 | 0,637 | 1,275 | 30 000 |
| 100 | 70,794 | 1,165 | 3,023 | 60 000 |

Ces durées concernent les algorithmes Node.js, sans rendu Canvas ni rafraîchissement de l'interface. Elles constituent un résultat de mesure, pas une garantie sur toute machine. Les tests ajoutent un seuil large pour détecter les régressions majeures, sans imposer une fréquence d'affichage.

## Prédiction des conflits

La prédiction utilise des intervalles continus d'occupation des arêtes et des intersections, avec la vitesse actuelle et l'état de maintien manuel. Elle évite de manquer un croisement entre deux instants d'échantillonnage. Les paramètres initiaux sont 45 m de séparation sur une arête et 30 m autour d'un nœud ; ils sont propres au simulateur et ne sont pas des normes de séparation opérationnelles. L'alerte indique l'entrée dans cette enveloppe, pas l'instant d'un impact géométrique.

Les tests couvrent séparément le face-à-face, le croisement, le rattrapage, le blocage par un avion maintenu, la présence d'un avion déjà arrivé, les arrivées décalées à un carrefour et l'absence de fausses alertes entre voies parallèles sans liaison topologique. Ils couvrent aussi les graphes déconnectés, les nœuds identiques, l'interdiction des raccourcis par piste, le maintien/reprise, le dépassement de destination par un grand pas et la répétabilité de 100 avions. **11 tests du noyau réussissent**, dont un test sur les données LFPG réelles et une comparaison indépendante des distances.

Le scénario de 100 avions a produit jusqu'à 122 paires en alerte au cours des 150 secondes, avec des observations des quatre catégories. Une paire peut être observée à plusieurs instants : ces compteurs ne représentent pas des accidents distincts. La génération aléatoire crée volontairement du trafic sans espacement ATC initial, et aucune résolution de conflit n'est automatique.

## Interface : contrôles en DOM virtuel

La suite complète contient **26 tests réussis** : 11 tests du noyau, 8 des adaptateurs et 7 contrôles de l'interface. `npm test` reconstruit d'abord le bundle Vite de production. Les tests d'interface exécutent ce vrai bundle avec les données LFPG locales dans jsdom, avec un Canvas simulé et des images d'animation avancées explicitement. Les commandes Canvas vérifient aussi l'absence de coordonnées non finies.

Les interactions vérifiées sont le calcul d'itinéraire et l'ajout, la pause/reprise, la vitesse ×10, le maintien d'un avion, son retrait, la génération de 100 avions, leur sélection, les alertes, la réinitialisation, les calques, le zoom/recentrage et l'audit. Les imports JSON sont exercés avec une capture fraîche, un JSON mal formé, un stand inconnu, une capture périmée et une capture expirant après son ouverture. Préparer une affectation sélectionne un stand et une piste sans créer ni déplacer d'avion.

**Ce contrôle ne constitue pas une vérification visuelle dans un navigateur.** L'interface de navigateur CUA n'était pas disponible pendant cette exécution. Les tests ne vérifient donc pas la disposition CSS réelle, le rendu des polices ou les gestes dans un moteur de navigateur ; une ouverture locale reste nécessaire pour ces points.

## Limites

La [planche géographique](lfpg-route-review.png) a été produite à partir des surfaces source et de huit parcours Dijkstra indépendants, puis inspectée visuellement. Cette revue a révélé l’inversion des extrémités 09L/27R par rapport à l’ordre du libellé OSM ; leurs positions sont désormais résolues à partir des seuils AVISO nommés et leur orientation est testée. Les parcours figurent dans [review-routes.json](review-routes.json). Reproduction après installation de Pillow : `python scripts/render_route_review.py`. Cette planche n’est pas une capture du Canvas de l’application.

Les données OSM et AVISO peuvent être incomplètes, anciennes ou présenter des divergences. Le prototype privilégie une absence explicite de route à une liaison inventée. Les tests garantissent le respect de la géométrie importée et le fonctionnement des algorithmes sur ces données ; ils ne certifient ni la totalité des infrastructures réelles ni la validité opérationnelle d'un chemin.

Les vitesses sont constantes entre interventions manuelles ; les gabarits d'avions, les virages, les sens de circulation, les autorisations ATC, les limites de piste et les règles locales ne sont pas modélisés. Les adaptateurs acceptent des instantanés de données ; l'intégration EuroScope et le transport natif C++ restent à réaliser. La vérification visuelle du Canvas est décrite dans le compte rendu général du prototype lorsqu'elle est disponible.
