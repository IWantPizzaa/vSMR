# Validation du routage par axes principaux

Ce contrôle concerne le mode effectivement utilisé par l’interface : `TaxiFlowPolicy` ajouté au planificateur opérationnel. Le rapport précédent sur les seules contraintes directionnelles ne prouve pas la validité de ces nouveaux itinéraires.

## Méthode et hiérarchie des sources

Le calcul conserve les interdictions AIP, les fermetures sélectionnées, les restrictions de virage et les points d’attente. La couche de sens nominaux rapprochée des MANEX et des GMC complète ces contraintes. Les anciennes portées AVISO marquées `authoritative: false` ne sont plus traitées comme des interdictions dans ce mode : elles avaient notamment inversé ou étendu certains sens de N, F et E. Le mode de référence sans politique de flux reste disponible pour les comparaisons automatiques, sans servir de repli à l’application.

La recherche utilise des étapes de raccordement publiées et un objectif lexicographique :

1. Réduire le trajet sur une aire intermédiaire ou dans une aire déjà quittée.
2. Réduire les sorties puis reprises d’un axe principal via une aire.
3. Minimiser la distance physique additionnée des coûts bornés de courbure cumulée et de changement d’axe principal.
4. Départager les coûts égaux par la distance physique.

Les deux premiers critères restent lexicographiques. Le troisième utilise des équivalents métriques documentés dans [ROUTE-QUALITY-VALIDATION.md](ROUTE-QUALITY-VALIDATION.md) ; ils constituent une préférence de simulation, pas une règle AIP. La distance affichée et utilisée pour le déplacement reste la somme des longueurs réelles des arêtes. `preferredAxisIds` décrit les alternatives admissibles pour la convergence ; l’ordre de cette liste ne constitue pas un classement numérique des taxiways. Les voies intérieures des baies de dégivrage cartographiées sont exclues des parcours normaux, y compris lors du rejeu dans le simulateur.

Les raccordements G3/G4 déclarés comme connecteurs restent utilisables pour les trajets locaux entre 2B/2D et 2F. Une correspondance avec un polygone d’aire plus large ne transforme pas ce connecteur partagé en transit interdit. Les transferts au sein d’une aire partagée ne sont pas artificiellement envoyés sur N ou F.

## Contrôles indépendants

Les tests de `tests/manex-regressions.test.ts` utilisent les directions cardinales et les voies source relevées dans les documents, plutôt que de relire les directions générées pour produire leur résultat attendu. Ils examinent les portions principales complètes de N et F dans les deux configurations et E face ouest, puis des parcours réels. Ils contrôlent aussi la première sortie TF d’un départ, la dernière entrée TF d’une arrivée, P4/P5 pour J et le franchissement de la limite physique de l’aire FedEx par B au départ et M à l’arrivée.

Les tests du moteur comprennent un chemin court passant par une aire étrangère face à un chemin plus long mais cohérent, un portail nominal indisponible, une tentative de sortie par le mauvais portail suivie d’une boucle pour toucher le bon, la conservation des interdictions permanentes, un transfert local, les références périmées et les stands sans aire connue. Le simulateur rejoue les arêtes, les portails et les étapes ; copier des métadonnées d’un bon parcours sur un mauvais ne suffit pas à l’autoriser.

Un stand sans correspondance d’aire reste explicitement à vérifier, même si une politique générale de convergence peut fournir un trajet. La génération automatique exclut ces demandes et les autres parcours marqués `review`. Une sélection manuelle peut afficher un tel parcours avec ses réserves ; elle ne devient pas une preuve de conformité du raccordement local.

## Exécution et rapports

Depuis `smart-ground/` :

```powershell
npm.cmd test
npm.cmd run validate:flows
```

Le validateur écrit [flow-validation-results.json](flow-validation-results.json) et [flow-review-routes.json](flow-review-routes.json). Il vérifie les empreintes exactes du graphe, du catalogue d’exploitation et de la politique de flux, et refuse un changement de fichier pendant l’exécution. Les profils temporaires sont résolus par le même module que l’interface.

Il compare les anciens et nouveaux parcours sur des stands de plusieurs zones, conserve les nœuds et arêtes des deux versions pour la revue visuelle, puis génère 10, 25, 50 et 100 avions dans les deux configurations et les trois mouvements. Chaque position est comparée à l’interpolation de son segment source. Une traversée physique d’un axe de piste requiert un point d’attente source antérieur, associé à la même piste et non déjà consommé par une traversée précédente. Les prédictions d’un avion arrêté devant une clairance ne dépassent pas ce verrou.

Les résultats chiffrés et les durées de la dernière exécution figurent dans le rapport JSON. Les mesures concernent les algorithmes Node.js ; elles excluent le rendu Canvas et les performances du navigateur.

## Résultats de l’exécution finale

Le catalogue de corridors figé pour cette exécution porte l’empreinte SHA-256 `9228e0c4e9293426c2318ea032ce5ac07fc331595201b8c6eeaa6916cdf8def3`. Graphe : `4597cfcb70d555ca801427a378d7f8fca297e55677d3062faed0c1718ed5b9c5` ; exploitation : `b6895a31356631254f3afde5b9e0624f7462203dd328f5d1660467f50784aa83` ; restrictions aéronefs : `02e28bb4f889c615533bdec17f87f21b360b13990635f6fb2ef441c29b489eda`. Le profil courant est A320, 35.8 m. Les profils de travaux et flags effectifs sont `sup188-2026` ; les accès PPR ne sont pas approuvés.

Les **48 trajets représentatifs** ont tous un parcours. Les **24 scénarios** de 10, 25, 50 et 100 avions ont généré le nombre demandé. Le contrôle examine **1,158 parcours**, **666,000 positions exactes sur les arêtes** et **386 traversées physiques de piste**, toutes protégées par un point d’attente source antérieur. Aucun parcours normal ne traverse les 67 arêtes intérieures de dégivrage exclues.

Les six générations de 100 avions contiennent uniquement des parcours marqués `nominal`. Les arrivées s’arrêtent à leur clairance de piste ; les mesures ne représentent donc pas 100 arrivées roulant librement. Les durées détaillées restent dans le [rapport JSON](flow-validation-results.json). Elles concernent les algorithmes Node.js, avec d’autres contrôles en parallèle, et ne constituent pas un benchmark du navigateur.

Comparaison avec le moteur directionnel historique, distinct du mode MANEX précédent :

| Contexte et parcours | Historique | Actuel | Axes principaux actuels |
| --- | ---: | ---: | --- |
| Est, F02 vers 09R/D3 | 5 161,701 m | 6 262,725 m | E → F → D |
| Est, 09L/K6 vers F02 | 4 412,032 m | 4 276,315 m | Q → N → E |
| Ouest, B24 vers 27L/Q4 | 3 340,934 m | 3 446,629 m | N → B |
| Ouest, 27R/K3 vers F02 | 4 841,418 m | 5 062,268 m | D → F → E |

La longueur historique n’est pas une référence d’autorisation lorsque les anciens sens diffèrent du catalogue corrigé. La [revue des détours](ROUTE-QUALITY-SOURCES.md) et la [revue des accès aux points d’attente](HOLDING-TURN-SOURCES.md) conservent séparément les vrais parcours avant leurs corrections respectives, sans reconstruire ces références avec le nouveau code.

La validation complémentaire [ROUTE-QUALITY-VALIDATION.md](ROUTE-QUALITY-VALIDATION.md) ajoute le balayage de tous les accès de départ ouverts depuis huit origines, les travaux activés ou désactivés et les contrôles de courbure. La suite complète couvre le moteur, les régressions MANEX, les trois cas utilisateur exacts, les adaptateurs et l’interface en DOM simulé.

## Portée

Les chemins nominaux peuvent différer d’une clairance ATC coordonnée. Les variantes de repoussage, les restrictions liées au type ou au gabarit et la totalité des décisions tactiques ne sont pas ajoutées par cette politique. Les tests vérifient les itinéraires produits sur les instantanés inclus ; ils ne certifient pas tous les marquages physiques ni les NOTAM en vigueur. La représentation des zones et des portails reste limitée par les correspondances documentées dans [CORRIDORS.md](CORRIDORS.md).
