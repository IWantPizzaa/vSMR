# Direction des voies : projection des flèches LFPG

**Référence historique de projection.** Le mode MANEX courant n’applique plus ces règles `authoritative:false` comme des interdictions. Les portées locales étaient insuffisantes pour décrire les grands flux. Les sens corrigés et la convergence active sont documentés dans [CORRIDORS.md](CORRIDORS.md) et [FLOW-VALIDATION.md](FLOW-VALIDATION.md).

`public/data/lfpg-directions.json` conserve les flèches des couches **East Arrows** et **West Arrows** du fichier AVISO local et leur rapprochement avec les segments OSM du graphe. Il complète les restrictions AIP du fichier `lfpg-operations.json` ; ce n'est pas un catalogue certifié de tous les sens de circulation réels.

## Résultats de la projection

| Contrôle | Face est | Face ouest |
| --- | ---: | ---: |
| Flèches source conservées | 89 | 97 |
| Flèches centrales | 77 | 85 |
| Flèches centrales projetées sans ambiguïté | 77 | 83 |
| Flèches T1 colorées conservées pour référence | 12 | 12 |
| Segments de taxiway concernés | 962 | 1 086 |
| Longueur des segments concernés | 34,09 km | 39,61 km |
| Segments de taxiway sans règle issue de ces flèches | 6 326 | 6 202 |

Le nombre de segments dépend du découpage fin OSM et ne représente pas une proportion d'infrastructure certifiée. Les 2 051 règles incluent des contextes arrivée/départ distincts sur un même segment. Les restrictions permanentes et les traversées de piste sont traitées séparément par le catalogue AIP. Une absence de règle dans cette couche ne signifie pas qu'une voie est opérationnellement libre de restrictions.

Deux flèches centrales ouest ne disposent pas d'une correspondance satisfaisant simultanément la distance et l'angle : indices source 3 et 25 du groupe `LFPG-afb8597aeeddf77f136b`. Elles restent visibles et figurent dans `diagnostics.unmatchedArrows`. Aucun lien ni sens de circulation ne leur est inventé.

## Sources et valeur des indications

Le fichier `data-sources/lfpg-aviso.geojson` déclare une origine **local GNG**, licence GPL-3.0. Il ne donne pas de date AIRAC pour ses flèches. Son SHA-256 est conservé avec celui du graphe. Ces flèches constituent donc une couche supplémentaire datée de façon insuffisante pour démontrer à elles seules une conformité IRL actuelle.

Les cartes officielles [SIA AD 2 LFPG GMC 01, face est](https://www.sia.aviation-civile.gouv.fr/media/dvd/eAIP_29_OCT_2026/FRANCE/AIRAC-2026-10-29/html/eAIP/Cartes/LFPG/AD_2_LFPG_GMC_01.pdf) et [GMC 02, face ouest](https://www.sia.aviation-civile.gouv.fr/media/dvd/eAIP_29_OCT_2026/FRANCE/AIRAC-2026-10-29/html/eAIP/Cartes/LFPG/AD_2_LFPG_GMC_02.pdf), publiées le **11 juin 2026** dans le cycle effectif du **29 octobre 2026**, ont été lues visuellement. Elles différencient les mouvements ARR et DEP. Cette distinction interdit de convertir aveuglément chaque flèche en sens unique universel.

- Les flèches de l'axe nommé `E` dans la source sont utilisées pour le contexte départ uniquement. Elles n'interdisent pas à elles seules les flux d'arrivée opposés indiqués par les cartes.
- En configuration ouest, les flèches source sur `R` et `T` vers l'est concernent les départs, celles vers l'ouest les arrivées. Trois segments de `T` reçoivent ainsi deux sens opposés selon le mouvement : ce n'est pas une contradiction ni une fermeture.
- Les 24 flèches T1 vertes/brunes décrivent des variantes **GREEN/RED PUSH/PULL** choisies par le contrôle. Elles restent indicatives et n'imposent pas simultanément deux circuits incompatibles.

Les noms OSM ne sont pas tous les noms AIP actuels, notamment dans les aires du terminal 2E. Le rapprochement géométrique ne renomme pas silencieusement ces voies. Les restrictions AIP doivent être localisées par leurs intersections et leurs positions publiées, et non appliquées aveuglément à tout segment portant un ancien identifiant.

## Méthode et inférences explicites

1. Extraire la tige et la pointe de chaque flèche, en séparant les deux ailes de la pointe. Les 186 polylignes originales sont conservées pour l'affichage, y compris les variantes T1.
2. Chercher un segment de taxiway existant à moins de 12 m du milieu de la tige, avec un écart angulaire maximal de 35 degrés. Les candidats concurrents de voies séparées à score équivalent restent non résolus.
3. Orienter le segment dans le sens de la tige. Le sens `forward` correspond exactement à `edge.from → edge.to`, et `reverse` au sens opposé.
4. Étendre localement cette indication sur la même voie connectée, tant qu'une seule continuation de même nom est géométriquement cohérente. Pour une voie sans nom, rester dans la voie OSM source. Arrêter aux branches ambiguës, aux changements d'axe trop importants ou à 500 m de parcours de chaque côté de la flèche.
5. Conserver les divergences entre flèches dans les diagnostics et ne pas ajouter de règle sur une portion réellement contradictoire pour le même mouvement. Ne créer aucune géométrie de raccordement.

**La limite de propagation de 500 m est un paramètre d'inférence du prototype, pas une frontière publiée par le SIA.** Les règles portent `authoritative: false` et `mapping: bounded_same_lane_continuity`. Les restrictions précisément sourcées dans l'AIP prennent priorité lors de la fusion. Une relecture humaine complète des portées et des virages demeure nécessaire avant de qualifier l'ensemble des parcours d'identiques aux roulages IRL.

Les tags OSM `oneway` ne sont pas appliqués globalement : plusieurs concernent des lignes d'entrée au stand dont le départ exige un repoussage. Les considérer comme des interdictions de départ sans modèle de repoussage serait incorrect. Les sorties de piste interdites à l'entrée sont prises dans l'AIP plutôt que déduites de ces tags.

## Reproduction et vérification

Après la génération du graphe LFPG :

```powershell
python scripts/map_operating_directions.py
```

Shapely est le seul prérequis pour cette commande. Elle utilise les instantanés locaux et ne télécharge rien. Pour produire également la [planche de revue géographique](lfpg-direction-review.png), installer Pillow puis exécuter :

```powershell
python scripts/map_operating_directions.py --render-review
```

La planche a été inspectée : les deux configurations couvrent les voies principales sans raccordement nouveau, les deux projections absentes sont signalées et les variantes T1 restent indicatives. Elle présente les données, pas une capture de l'interface du simulateur.

Un contrôle de joignabilité indépendant sur cette seule couche directionnelle a confirmé que les 451 stands du réseau principal rejoignent chacun des deux anciens accès de piste au départ de leur configuration, sans déplacement le long des pistes. Ce contrôle précède l'ajout des règles AIP, des nouveaux points d'attente et des autorisations manuelles de traversée ; la validation de ces contraintes combinées appartient aux tests opérationnels finaux.
