# Validation des règles de roulage LFPG

**Référence du mode précédent sans politique MANEX.** Ce rapport reste utile pour les contraintes AIP et les comparaisons, mais il ne valide pas le choix actuel des cheminements. Voir [FLOW-VALIDATION.md](FLOW-VALIDATION.md) pour le mode actif, les corrections et les parcours avant/après.

Les vérifications portent sur le réseau géographique livré et le catalogue de contraintes publié, avec les mêmes profils que l’application. Elles n’établissent pas l’exhaustivité des procédures réelles ni des NOTAM du jour. Les sources, leurs dates, les restrictions cartographiées et les six références encore non résolues sont décrites dans [OPERATIONS.md](OPERATIONS.md).

## Reproduction et instantanés

Depuis `smart-ground/` :

```powershell
npm.cmd test
npm.cmd run validate:operations
```

La première commande construit le véritable bundle Vite avant les tests. La seconde utilise les fichiers locaux, sans accès réseau, et produit [operations-validation-results.json](operations-validation-results.json) ainsi que [operating-review-routes.json](operating-review-routes.json). Le validateur refuse de terminer si le catalogue change pendant son exécution.

Instantanés de la mesure ci-dessous :

- Graphe LFPG : SHA-256 `4597cfcb70d555ca801427a378d7f8fca297e55677d3062faed0c1718ed5b9c5`.
- Catalogue des règles : SHA-256 `b6895a31356631254f3afde5b9e0624f7462203dd328f5d1660467f50784aa83`.
- Profil par défaut : **SUP 188/26 activé** ; **SUP 151/26 phase 2 désactivé**, son activation dépendant d’une confirmation NOTAM.
- Accès PPR désactivés en l’absence d’accord préalable simulé.

L’interface, les tests et le validateur partagent `resolveOperatingManifest`. Les fermetures de stands et d’arêtes des profils sélectionnés sont donc appliquées au même scénario. Une série de tests séparée active les deux profils et vérifie leurs fermetures ; elle ne suppose pas que la phase conditionnelle est effectivement active dans le monde réel.

## Directions, mouvements et itinéraires

La configuration face est ou face ouest et le contexte départ, arrivée ou transfert font partie du calcul. Les mouvements opposés publiés sur une même voie peuvent dépendre de ce contexte : ils ne sont pas fusionnés en une interdiction contradictoire. Une restriction commune continue de s’appliquer à tous les mouvements. Les sources de chaque contrainte appliquée sont conservées dans les métadonnées du parcours, avec les segments dont le sens demeure non vérifié.

La recherche tient compte de l’arête d’arrivée à chaque nœud et de la phase parking/taxiway/parking. Elle respecte les interdictions de virage, ne réutilise pas les lignes de stands comme raccourcis intermédiaires et conserve la continuité d’un segment source aux intersections synthétiques dépourvues de virage explicitement cartographié. Une absence de chemin est retournée sans relancer une recherche libre.

Une implémentation Dijkstra indépendante reconstruit directement les permissions depuis le JSON : directions, contexte, virages, demi-tours et accès aux lignes de parking. **72 paires d’origine/destination** sont comparées dans les six contextes : 62 possèdent une route, de distance identique à A* ; les 10 autres sont injoignables dans les deux implémentations. Les trajets n’empruntent aucune arête de piste.

Le validateur examine aussi la joignabilité de chaque accès de départ par défaut depuis tous les stands ouverts possédant un point de graphe. **416 des 449 stands considérés** peuvent rejoindre chacun de ces accès dans les deux configurations. Le détail individuel des 33 stands sans chemin est conservé dans le rapport. Les 523 noms importés comprennent également 63 stands sans nœud ; 11 stands possédant un nœud sont fermés par le profil de travaux par défaut. Ces catégories ne doivent pas être présentées comme une couverture opérationnelle complète.

Les accès par défaut vérifiés sont D3/D4/D5 pour 09R et T3/T4/T5/T6 pour 08L face est ; Q4/Q5 pour 27L et T9/T10/T11/T12 pour 26R face ouest. Ils correspondent à des points d’attente source, et non aux anciennes destinations situées sur l’axe d’une piste.

## Pistes, points d’attente et priorités

Une traversée planifiée ne constitue pas une autorisation. La simulation s’arrête au point d’attente source applicable ; une action manuelle distincte est nécessaire pour reprendre. Le bouton de maintien/reprise ordinaire ne contourne pas ce verrou. La prédiction s’arrête également à ce point : elle ne réserve pas artificiellement un carrefour après une traversée encore non autorisée.

Un contrôle indépendant examine les parcours testés aux nœuds partagés avec les axes des quatre pistes nommées. Il vérifie un changement de côté de l’axe, puis exige un point d’attente source orienté sur la route, associé à la même piste et strictement situé avant la traversée. Une protection ayant déjà servi à une traversée précédente ne suffit pas pour la suivante. **393 traversées physiques ont été contrôlées sur les 1 172 parcours examinés ; aucune ne manque de protection préalable.** Ce résultat porte sur ces parcours, pas sur l’exhaustivité de toutes les arêtes brutes ou de futurs jeux de données. Le validateur échoue si un parcours testé traverse une piste sans protection.

Les priorités publiées sont appliquées uniquement aux branches d’arrivée exactement cartographiées. Le contrôleur de priorité utilise l’occupation temporelle continue : l’avion concerné s’arrête sur son parcours avant le carrefour, puis reprend après le passage du trafic prioritaire. Cela ne délivre aucune clairance de piste. Les marges initiales, 45 m avant le carrefour et 12 s entre occupations, sont des paramètres du simulateur, pas des valeurs de séparation publiées.

Les tests couvrent la configuration, les branches et le type de mouvement prioritaires, les passages suffisamment décalés, les voies parallèles, le maintien manuel et les points d’attente non libérés. Ils vérifient aussi l’intégration réelle dans `AircraftSimulator` : arrêt, prévision arrêtée au bon endroit sur une arête, reprise après disparition de la priorité et conservation d’un maintien devenu explicitement manuel. Les grands pas temporels sont subdivisés lorsque des priorités sont actives.

## Simulation et performances

Les tailles **10, 25, 50 et 100 avions** ont été générées dans chaque configuration et chaque contexte, soit 24 scénarios. Tous ont atteint le nombre demandé. Chaque scénario a parcouru 150 secondes simulées par pas de 0,25 s. **666 000 positions** ont été comparées à l’interpolation exacte de leur arête ; aucune sortie du graphe n’a été observée. Au total, **1 172 parcours** ont été contrôlés, comparaisons et générations comprises.

Mesures de l’exécution locale pour les scénarios de 100 avions, en millisecondes :

| Configuration | Mouvement | Génération | Mise à jour moyenne, priorités incluses | Prédiction moyenne, 120 s | Prédiction maximale |
| --- | --- | ---: | ---: | ---: | ---: |
| Est | Départ | 128,679 | 0,427 | 0,987 | 1,766 |
| Est | Arrivée | 329,198 | 0,014 | 0,372 | 0,480 |
| Est | Transfert | 236,489 | 0,397 | 0,775 | 1,442 |
| Ouest | Départ | 248,363 | 0,407 | 0,790 | 1,115 |
| Ouest | Arrivée | 252,285 | 0,013 | 0,617 | 0,818 |
| Ouest | Transfert | 247,599 | 0,347 | 0,686 | 0,903 |

Les 100 arrivées de chaque scénario restent en attente d’une décision manuelle à un point précédant la traversée de la piste intérieure. Leur faible coût de mise à jour reflète donc principalement cet état arrêté ; il ne mesure pas 100 avions roulant librement en permanence. Les scénarios homogènes ne constituent pas une mesure du débit mixte d’un aéroport. Les tests de priorité utilisent séparément des rencontres mixtes départ/arrivée contrôlées.

Ces mesures Node.js excluent le rendu Canvas et ne garantissent pas un taux d’images dans un navigateur. Les durées varient selon la machine et la charge ; les paramètres, routes et instantanés restent reproductibles.

## Vérification visuelle et limites

Le fichier [operating-review-routes.json](operating-review-routes.json) conserve trois routes représentatives par contexte, leurs nœuds, leurs arêtes, les noms des points et leurs points d’attente. La [planche de revue opérationnelle](lfpg-operating-review.png) montre les départs et arrivées des deux doublets dans les deux configurations, avec le profil SUP 188/26. Ses contraintes et empreintes sont contrôlées avant rendu. Il s’agit d’un rendu géographique séparé, pas d’une capture du navigateur.

Le build et les interactions du véritable bundle sont vérifiés en DOM simulé ; le contrôle visuel interactif dans un navigateur reste indisponible dans cet environnement. Les flèches AVISO locales sont supplémentaires et non datées, certaines références de la publication ne disposent pas d’une correspondance géographique sûre et les décisions ATC non publiées ne sont pas déduites. Les restrictions liées au type ou au gabarit de l’appareil restent volontairement hors périmètre.
