# Réseau LFPG : sources, construction et limites

Le jeu livré couvre l'emprise entière de LFPG avec des géométries réelles. Il est autonome : aucune requête réseau n'est nécessaire au démarrage du simulateur. Les fichiers source sont conservés dans `data-sources/`, les données utilisables dans `public/data/lfpg.json`.

## Inventaire vérifié

| Donnée | Nombre |
| --- | ---: |
| Voies OSM de type taxiway | 779 |
| Voies OSM de type parking_position | 478 |
| Voies OSM de type runway, y compris seuils décalés et FATO | 7 |
| Nœuds du graphe, intersections géométriques comprises | 7 650 |
| Arêtes utilisables, issues de segments source | 8 487 |
| Intersections réelles ajoutées par découpage des segments | 88 |
| Surfaces AVISO, dont 74 bâtiments | 430 |
| Étiquettes de stands AVISO préservées | 492 |
| Stands de la configuration Ramp Agent préservés | 448 |
| Noms de stands distincts, toutes sources réunies | 523 |
| Stands reliés à un point existant du graphe | 460 |
| Stands reliés au composant principal | 451 |
| Stands sans itinéraire vers le composant principal | 72 |
| Points d'attente OSM préservés | 158 |
| Destinations associées aux 8 extrémités des 4 pistes | 8 |

Les 72 stands indisponibles comprennent 63 noms sans point de graphe identifié et 9 dans un composant secondaire. Le simulateur conserve leurs positions et ne doit pas calculer de liaison fictive pour les rendre accessibles. Les noms ressemblants, par exemple `R5` et `R05`, restent distincts.

## Sources épinglées

1. **OpenStreetMap contributors**, instantané Overpass daté `2026-11-12T18:28:04Z`, sous [ODbL](https://www.openstreetmap.org/copyright). Requête incluse dans les métadonnées ; boîte d'emprise `[48.983, 2.485, 49.032, 2.623]`, qui englobe les quatre pistes, les terminaux, le fret et les parkings éloignés. Le téléchargement sélectionne tous les taxiways, parking_position, runway et holding_position de l'emprise, puis tous leurs nœuds. Les tags et l'identité des nœuds sont conservés dans l'instantané source.
2. **AVISO LFPG fourni par ce dépôt vSMR**, copié de `vSMR/data/AVISO/LFPG.geojson`. Sa métadonnée déclare `geometry_source: local GNG` et `geometry_license: GPL-3.0`. Le fichier de licence complet est conservé dans le dépôt à `vSMR/data/Licenses/France-Ground-Layouts.txt`. Cette source fournit les surfaces, bâtiments, étiquettes et marquages de référence.
3. **[Configuration LFPG Ramp Agent de la French vACC](https://github.com/vaccfr/RampAgent-Config)**, révision `63513c01e3989d6f1f02b3fa366201939be3cb5c`, fichier `airports/LFPG.json`. Les 448 stands et leur terminal sont préservés. Aucun fichier de licence explicite n'a été trouvé dans ce dépôt à cette révision : les données sont conservées pour ce prototype local demandé ; leurs conditions de redistribution restent à clarifier. La licence du code C++ Ramp Agent ne s'applique pas automatiquement au dépôt de configuration distinct.

Les empreintes SHA-256 de chaque fichier source figurent dans les métadonnées de `lfpg.json` et dans `connectivity-report.json`. La source Ramp Agent reste une source de positions et d'identifiants : aucune logique d'attribution de stand n'est reproduite.

## Construction du graphe

Le graphe utilise les segments de centre de voie OSM, et non les contours des surfaces AVISO. Chaque arête conserve son identifiant de voie source. L'identité d'un nœud OSM est respectée ; les nœuds ayant exactement les mêmes coordonnées source sont fusionnés. Les segments qui se croisent réellement au même niveau sont découpés à leur intersection. Les tags `layer` sont pris en compte pour éviter de connecter deux niveaux différents. Les clés de coordonnées d'intersection sont arrondies au micromètre pour absorber les erreurs de calcul flottant uniquement.

**Aucun segment de raccordement n'est inventé.** Les composantes distinctes ne sont pas reliées par leur simple proximité. Les segments restent entièrement contenus dans leur segment source après découpage. Le contrôle automatique vérifie leur filiation et la conservation des longueurs à 0,01 mm près.

Les positions sont exprimées en mètres locaux, avec `x` vers l'est et `y` vers le nord :

```text
x = (longitude - 2.5479) × 111320 × cos(49.0097°)
y = (latitude  - 49.0097) × 111320
```

Il s'agit d'une projection locale adaptée à ce prototype, à conserver identique entre routage, affichage et simulation. Le futur portage C++ pourra utiliser une projection géodésique plus précise, à condition de conserver une métrique cohérente.

Un stand est associé à un nœud déjà présent sur un point ou une ligne de parking OSM portant **exactement le même nom**, avec une vérification de cohérence spatiale à 60 m maximum. Ce seuil ne crée aucune liaison : il sert à refuser les associations de noms incohérentes. Le départ de l'avion se fait au point réel du graphe, sans déplacement direct jusqu'à l'étiquette AVISO ou Ramp Agent. Les coordonnées d'origine sont conservées avec leur source.

Les 1 458 lignes de référence AVISO ou segments exclus restent accessibles dans `referenceLines`. Elles comprennent des limites, marquages et lignes de stands qui ne sont pas toutes des axes de roulage ; elles ne deviennent pas implicitement des arêtes de routage. Les flèches directionnelles sont exclues de cette première version bidirectionnelle.

## Connectivité et pistes

Le composant principal comprend 7 601 nœuds si les arêtes de piste sont comptées, ou 7 571 nœuds dans le graphe de roulage sans déplacement le long des pistes. Les 451 stands du composant principal peuvent tous rejoindre les huit destinations de piste sans emprunter d'arête de piste. Les autres petits composants et les nœuds de seuil isolés sont conservés et signalés.

Les destinations `08L`, `08R`, `09L`, `09R`, `26L`, `26R`, `27L` et `27R` désignent des **intersections réelles de taxiway et piste**, proches de l'extrémité considérée. Elles ne représentent ni le seuil de piste, ni un point d'attente réglementaire, ni une autorisation d'entrée sur piste. Les positions des seuils nommés AVISO sont conservées séparément et identifient le côté physique correct : l'ordre des nœuds d'une voie OSM ne correspond pas nécessairement à l'ordre des noms de piste. Pour le roulage normal, le planificateur exclut les arêtes de piste.

## Bâtiments et données manquantes

Les segments source sont contrôlés contre l'intérieur des 74 polygones de bâtiment AVISO, après retrait d'une bordure d'un mètre pour ne pas rejeter un simple contact de contour. Deux segments de parking (`V03` et `V01/V02`) pénètrent ces intérieurs de plus d'un mètre. Ils sont exclus des arêtes utilisables et conservés en géométrie de référence avec leur motif d'exclusion. Cette incohérence entre sources doit être résolue avec une source à jour avant de réactiver les segments ; aucun contournement n'a été dessiné.

Les noms sans itinéraire vers le réseau principal sont listés individuellement dans `public/data/connectivity-report.json`, champ `diagnostics.unavailableStands`. Ils concernent notamment plusieurs variantes du secteur I, des parkings isolés et des désaccords d'identifiants entre les sources. Aucun stand n'est supprimé pour masquer ces limites.

La géométrie n'est pas une carte opérationnelle certifiée. Les dates de relevé OSM varient, et les deux sources peuvent refléter des états différents de l'aéroport. Les règles de sens, gabarit, configuration des pistes, clairances et procédures LFPG ne sont pas modélisées dans cette version.

## Régénération et vérification

Depuis `smart-ground/` :

```powershell
python -m pip install -r scripts/requirements-data.txt
python scripts/import_lfpg.py
python scripts/validate_lfpg.py
```

Ces commandes utilisent les instantanés conservés et ne nécessitent pas le réseau après installation de Shapely. Pour remplacer explicitement un instantané :

```powershell
python scripts/import_lfpg.py --refresh-osm
python scripts/import_lfpg.py --refresh-ramp
python scripts/import_lfpg.py --refresh-aviso
```

Après une actualisation, régénérer et contrôler les rapports ainsi que les itinéraires. `--refresh-ramp` récupère toujours la révision épinglée ; changer `RAMP_COMMIT` est une décision de mise à jour explicite.

`validate_lfpg.py` vérifie les 8 487 arêtes, leur métrique, leur filiation source, la conservation des segments découpés, l'absence de traversée de bâtiment selon le critère décrit, la présence de tous les stands AVISO/Ramp Agent et la joignabilité des huit pistes. Son résultat est enregistré dans `public/data/geometry-validation.json`. Les huit itinéraires représentatifs testés mesurent de 1,4 à 9,5 km ; ils sont calculés exclusivement sur les axes de roulage source.

La revue visuelle indépendante est enregistrée dans `docs/lfpg-route-review.png` avec les huit itinéraires, les bâtiments et les stands indisponibles. Elle a permis de vérifier et de corriger l'association physique `09L/27R`, dont la voie OSM était orientée dans l'ordre opposé à son nom. Les huit extrémités font également l'objet d'un contrôle automatique d'orientation. Cette image est un rendu des données géographiques, pas une capture du navigateur.

Pour la régénérer, installer Pillow si nécessaire puis exécuter `python scripts/render_route_review.py`. Les itinéraires détaillés utilisés par ce rendu sont conservés dans `docs/review-routes.json` ; ils sont calculés avec Dijkstra indépendamment du module A* de l'application.
