# Enveloppes physiques orientées

Le détecteur [PhysicalConflictDetector](../src/core/PhysicalConflictDetector.ts) remplace les seules distances génériques de 45 m/30 m dans l'interface. Le détecteur historique reste disponible pour ses tests de référence. Le mode radar conserve confirmation, ancienneté des rapports, hystérésis et budgets d'incertitude ; seule la géométrie sous-jacente change.

## Dimensions et provenance

| Type | Longueur retenue | Largeur maximale de fuselage | Source locale primaire |
| --- | ---: | ---: | --- |
| A320 | 37,57 m | 3,95 m | [EASA.A.064, issue 62, page PDF 37](../data-sources/aircraft-profiles/easa-a320-tcds.pdf) |
| A388 | 72,73 m | 7,14 m | [Airbus A380 ACP, page PDF 29, figure 2-2-0](../data-sources/aircraft-profiles/airbus-a380-acp.pdf) |
| B744 | 70,67 m | 6,50 m | [Boeing 747-400 Rev F, page PDF 31, section 2.2.1](../data-sources/aircraft-profiles/boeing-747-400-rev-f.pdf) |
| B748 | 76,25 m | 6,50 m | [Boeing 747-8 Rev E, page PDF 15, section 2.2.1](../data-sources/aircraft-profiles/boeing-747-8-rev-e.pdf) |

L'envergure reste celle du profil de l'avion. Les longueurs de 873 autres entrées proviennent du catalogue natif existant [aircraft_types.json](../../vSMR/data/aircraft_types.json), figé dans [AircraftDimensions.ts](../src/core/AircraftDimensions.ts) par `python scripts/import_aircraft_dimensions.py`. Ces valeurs héritées ne sont pas présentées comme un nouveau contrôle indépendant des caractéristiques de chaque appareil. Les quatre sources primaires ci-dessus remplacent leurs valeurs natives arrondies.

Une longueur explicite est conservée dans le profil. Elle ne peut réduire une dimension connue : le détecteur garde la valeur connue et affiche un avertissement. Pour un type sans longueur connue ni longueur déclarée, la longueur de repli est `max(20 m, 1,5 × envergure)`, signalée comme **estimation à vérifier**. Une alerte issue de cette estimation reste de niveau avertissement.

## Forme représentée et limites

Pour les quatre profils principaux, l'empreinte est l'union de trois rectangles orientés : fuselage, bande ailes/moteurs et empennage horizontal. Les bandes contiennent volontairement des zones vides. Elles évitent toutefois de remplir tous les coins du rectangle longueur × envergure, ce qui réduirait inutilement les marges au voisinage du nez ou de la queue. Les limites des bandes sont des hypothèses géométriques explicites dans [AircraftGeometry.ts](../src/core/AircraftGeometry.ts), pas des cotes de silhouette publiées par les constructeurs.

Les types dont seule l'enveloppe longueur/envergure est connue conservent un **rectangle orienté**, identifié dans les métadonnées et le détail de l'avion. Le calcul ne prétend pas connaître leur silhouette exacte. Un cercle de rayon égal à la demi-envergure ne remplace pas cette empreinte : un tel cercle confondrait dimensions longitudinales et latérales.

La position reçue est traitée comme le centre géométrique de référence de l'empreinte. L'ancrage exact fourni par chaque client pilote devra être confronté aux replays et aux données du SDK ; l'enveloppe n'est pas un modèle certifié du nez, des trains ou du point GPS. Le modèle est horizontal en 2D : il ne résout pas les différences de hauteur ou les obstacles verticaux. Les conflits entre routes physiquement sécantes sont contrôlés même lorsque le graphe n'a pas de nœud commun ; cela ne prouve pas qu'une éventuelle séparation verticale est absente.

## Mouvement continu

Les intervalles de translation suivent les segments réels de la route. L'intersection de chaque paire de rectangles se résout analytiquement par axes séparateurs sur toute la durée de l'intervalle ; une collision entre deux messages distants de cinq secondes n'est donc pas perdue par un échantillonnage toutes les cinq secondes.

Chaque changement de tangente du graphe ajoute un balayage de rotation au nœud. Les orientations sont subdivisées en intervalles d'au plus 3°, et chaque intervalle possède une majoration géométrique de `2R × sin(Δangle/4)` autour de son orientation médiane. Cette majoration borne le mouvement de rotation entre deux orientations ; elle ne dépend pas de la fréquence des images. Aux angles brusques, ce balayage est volontairement conservateur : le graphe ne décrit pas nécessairement une courbe de braquage réellement praticable pour l'appareil.

Le cap du fuselage (`bodyHeading`) est distinct du cap du déplacement. Son décalage avec le mouvement est conservé lors des projections, notamment pour un repoussage. Une commande de maintien ne constitue pas une preuve qu'un avion réel s'est arrêté : la couche radar peut toujours projeter son mouvement observé sur son segment actuel, sans inventer son prochain virage.

## Marge et présentation

La marge d'activation par défaut est **2 m entre enveloppes**, et celle de disparition **5 m**. Ce sont des réglages du simulateur, pas des minima de séparation ATC. Les métadonnées distinguent le délai d'entrée dans cette marge du délai d'intersection des enveloppes elles-mêmes ; aucun des deux n'est annoncé comme une heure d'impact certaine. L'incertitude de surveillance élargit l'intervalle temporel et modifie la confiance ; elle n'augmente pas arbitrairement l'envergure de tous les avions.

Les vols programmés sont exclus avant activation. Les appareils arrêtés, arrivés ou non raccordés au graphe continuent d'occuper leur empreinte observée. Les projections nominales conservent les points d'attente non autorisés ; aucune nouvelle clairance n'est supposée.

Un rapport récent peut fournir une position exploitable sans association fiable à une voie. La confiance dans cette position est conservée séparément de celle du rapprochement au graphe. Si la position brute est validée, récente et d'incertitude bornée, le détecteur garde une empreinte stationnaire à ses coordonnées réelles ; il ne reprend pas un ancien segment ni une route supposée. Un chevauchement déjà présent peut donner immédiatement un **avertissement dégradé**, jamais une certitude de trajectoire ou une alerte critique fondée sur cette seule association inconnue. Les positions anciennes ou trop incertaines ne bénéficient pas de cette exception ; la disparition exige aussi des rapports indépendants confirmant l'écartement.

## Vérification

`tests/physical-conflicts.test.ts` contrôle les longueurs et les replis, l'approche nez-à-nez, les contacts entre rapports, les voies parallèles, les bouts d'aile, les coins vides, le repoussage, les rotations aux virages, les points d'attente et l'hystérésis indépendante de l'incertitude. Les alertes issues des replays réels restent des candidats à examiner : les fichiers ne fournissent ni vérité terrain de collision ni clairance complète de roulage.

Un essai local du détecteur avec 100 départs LFPG simultanés et un horizon de 120 s prend environ 20–23 ms après échauffement dans Node.js sur cette machine, avec 36 ms au premier passage. Il exclut le rendu, le SDK et les autres calculs ; ce n'est pas une garantie de performance de la DLL native ni un essai de plusieurs écrans.
