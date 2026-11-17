# Smart Ground · LFPG

Prototype HTML/TypeScript autonome pour vSMR v2 : carte géographique de Paris Charles de Gaulle, itinéraires A* contraints par configuration de roulage, trafic au sol et prévision des conflits. Tout le travail est isolé dans `smart-ground/`, sur la branche `Smart-Ground`. Le plugin C++ et ses configurations ne sont pas modifiés.


## Lancer le simulateur

Prérequis : Node.js **24 LTS, version 24.15 ou plus récente**, et npm. Version utilisée pour la validation : Node 24.20.0. Depuis la racine du dépôt, sous Windows PowerShell :

Les tests du parseur de replays demandent aussi Python 3.10 ou plus récent, sans paquet supplémentaire. Les replays d’origine restent externes au projet.

```powershell
cd smart-ground
npm.cmd ci
npm.cmd run dev
```

Ouvrir l’adresse locale imprimée par Vite, généralement **http://127.0.0.1:5173/**. Sous macOS/Linux, utiliser `npm` à la place de `npm.cmd`. Les données géographiques sont incluses : aucune API, clé ou requête cartographique externe n’est nécessaire pendant l’utilisation. L’installation des dépendances exige un accès au registre npm.

Pour produire et servir la version compilée :

```powershell
npm.cmd run build
npm.cmd run preview
```

Le dossier `dist/` contient la version statique. La servir par HTTP ; l’ouverture directe de `index.html` avec `file://` ne permet pas de charger les modules et le jeu de données.

## Utiliser

1. Choisir la configuration **face ouest** (défaut) ou **face est**, puis le mouvement **départ**, **arrivée** ou **transfert**, et le **profil aéronef**. A320 avec sharklets est le défaut ; A380-800, B747-400 et B747-8 sont proposés séparément. Un type ICAO et une envergure personnalisés sont aussi possibles. Un changement de configuration retire le trafic ; changer le profil ou le mouvement à planifier conserve les avions déjà présents et leur propre contexte.
2. Pour un départ, choisir un stand et un point d’attente de l’une des pistes de départ actives. Pour une arrivée, choisir un point d’attente réel après dégagement de l’extérieure, avant la traversée de l’intérieure, puis le stand attribué. Pour un transfert, choisir deux stands. Les points non raccordés sont désactivés.
3. Calculer l’itinéraire : axes principaux empruntés, distance réelle, virages aux jonctions, voies de raccordement, contraintes et points de clairance apparaissent avec le parcours. Le nombre de virages mesure les décisions aux embranchements ; il ne compte pas chaque sommet d’une courbe. Un cheminement nominal impossible est refusé ; le calcul ne revient pas silencieusement à un raccourci géométrique. Les cas demandant une relecture sont signalés.
4. Saisir un indicatif puis ajouter l’avion dans le mouvement sélectionné, ou générer **10, 25, 50 ou 100 avions**. Le générateur mélange toujours arrivées et départs en proportions équilibrées, indépendamment du mouvement choisi pour l’ajout manuel. Chaque génération remplace le trafic avec une nouvelle graine déterministe.
5. Démarrer, mettre en pause et sélectionner la vitesse ×1, ×2, ×5 ou ×10. Les avions progressent sur les segments de leur parcours et s’arrêtent à destination. À un point de traversée, sélectionner l’avion puis **Autoriser cette traversée · simulation** pour délivrer manuellement l’autorisation de ce seul franchissement. Reprendre le roulage ne contourne pas cette attente.
6. Sélectionner un avion sur la carte ou dans l’onglet Trafic pour inspecter son parcours, le mettre en attente, reprendre son roulage ou le retirer. Les avions programmés restent dans cette liste avec leur heure prévue ; ils apparaissent sur la carte et dans les prévisions de conflit seulement après leur activation. Réinitialiser retire tout le trafic. Les alertes ont un horizon réglable de 60 à 300 secondes.

Le trafic généré démarre progressivement : jusqu’à deux premiers mouvements, puis des mises en route réparties sur le maximum de **3 minutes** ou **12 secondes par avion**, soit environ 20 minutes pour 100 avions. La réutilisation d’un même nœud d’origine, y compris sous plusieurs noms de stand, impose au moins 90 secondes entre activations et attend le dégagement de la zone initiale. Ces durées règlent le rythme de la simulation ; elles ne représentent pas une séparation ATC.

Le sélecteur **Données de surveillance** utilise par défaut **Rapports espacés · 5 s**. Le simulateur fournit alors position, cap vrai et vitesse sol toutes les cinq secondes, avec un petit bruit déterministe allant jusqu’à 2 m par axe. La carte et les alertes exploitent une estimation entre les rapports, suivant la géométrie du graphe pendant au plus 10 secondes ; ensuite le suivi est figé et signalé périmé, puis perdu à 20 secondes. Une ambiguïté à un carrefour ou entre voies proches conserve une occupation prudente sans inventer une trajectoire certaine. Les images interpolées ne comptent jamais comme de nouveaux rapports. **Simulation exacte · référence** permet de comparer avec la position parfaite du simulateur.

Le suivi compare la voie et le sens observés à l’itinéraire attribué. Un écart clair doit être étayé par au moins deux rapports indépendants, espacés sur une durée d’au moins cinq secondes ; un cap fluctuant à l’arrêt ou un rapport ambigu ne suffit pas à confirmer un mauvais sens. Une fois l’écart confirmé, l’alerte identifie l’avion et conserve le parcours attendu et l’historique. Le recalcul cherche un raccordement autorisé vers cette affectation et ses axes principaux, en conservant configuration, profil aéronef, sens, interdictions de virage, carrefours encore imposés et points de clairance. L’avion attend une **reprise explicite** avant de suivre ce raccordement ; sans solution autorisée, il reste maintenu. Les traversées exigent toujours leur autorisation manuelle.

Pour attribuer un autre cheminement, sélectionner l’avion puis **glisser un cercle de son tracé vers une autre intersection**. Le trait provisoire et le point d’accrochage montrent le carrefour choisi ; relâcher applique le raccordement s’il est autorisé. **Échap** annule le déplacement avant de relâcher. Les carrefours déjà parcourus, la position actuelle et la destination restent fixes. Le calcul refuse les sens, virages ou gabarits interdits, sans inventer de liaison entre deux voies.

Le parcours standard est **vert** ; une route attribuée par déplacement devient **bleue**, avec le standard d’origine conservé en pointillés pour comparaison. Le Off Track utilise désormais la route attribuée et ses révisions. L’avion actif est maintenu après la modification pour reprise explicite ; un avion programmé conserve son heure. **Annuler la dernière modification** restaure l’affectation précédente tant que l’avion n’a pas emprunté la branche modifiée. La mise à jour de la référence radar conserve le dernier rapport reçu et son horodatage : une édition n’est pas une mesure supplémentaire. Les [règles de l’éditeur](docs/ROUTE-EDITING.md) précisent les contraintes conservées et les tests.

Par défaut, chaque avion suit son chemin assigné. **Simuler un mauvais virage** arme une seule erreur sur l’avion sélectionné : c’est un diagnostic direct du routage sur une position exacte, indépendant du délai de confirmation radar. Les tests de rapports bruités vérifient séparément la confirmation à partir des seules mesures espacées. Aucune erreur aléatoire n’est injectée. Un détour commis puis corrigé entièrement entre deux rapports peut rester inobservable.

La stabilisation des alertes de conflit tient compte de l’âge, de l’ambiguïté et de l’incertitude des suivis. Les prévisions incertaines sont limitées et signalées ; une menace proche crédible peut être publiée immédiatement. La confirmation des autres alertes et leur extinction utilisent des rapports indépendants et une hystérésis pour éviter les apparitions/disparitions à chaque image. Un avion hors itinéraire encore en mouvement n’est projeté que sur son segment observé ; une hypothèse ambiguë n’est pas prolongée artificiellement jusqu’à destination. Ces durées, amplitudes de bruit et marges sont des réglages du simulateur à confronter à des captures réelles, pas des garanties officielles d’EuroScope.

L’[adaptateur de rapports EuroScope](src/adapters/EuroScopeSurfaceAdapter.ts) définit déjà la projection, les sessions, les séquences et la conversion des données primitives. Le [contrat SDK vérifié](docs/EUROSCOPE-SURVEILLANCE.md) distingue notamment cap et trajectoire, vitesse transmise et repli, âge de réception et date de mesure. **Le flux C++ natif n’est pas encore connecté à cette chaîne** : l’interface utilise toujours des rapports synthétiques, sans prétendre que le portage vSMR est achevé.


Le bouton **Règles et sources** présente les documents datés et les restrictions appliquées. Il permet aussi de déclarer les baies SW1–SW4 occupées ou l’utilisation des axes orange/bleu pour activer leurs limites d’envergure conditionnelles. Par défaut, ces deux conditions sont désactivées et l’axe central est retenu. Les points soumis à PPR restent exclus tant que l’accord préalable simulé n’a pas été explicitement sélectionné.

Les [restrictions aéronefs](docs/AIRCRAFT-RESTRICTIONS.md) distinguent les interdictions propres à un type, les limites d’envergure, les virages interdits et les conseils de conduite. Une interdiction de tourner ne ferme pas toute la voie. Les cercles barrés du calque des virages signalent une jonction comportant au moins un mouvement interdit pour le profil courant ; les mouvements concernés sont détaillés dans les règles. Les [profils et leurs dimensions](docs/AIRCRAFT-PROFILES.md) sont sourcés séparément. Un profil inconnu reste à vérifier et ne peut pas être généré automatiquement.

Les restrictions liées aux travaux SUP 151/26 et SUP 188/26 sont exclues à la demande de l’utilisateur : aucune fermeture temporaire de voie ou de poste, ni limite temporaire code C sur E15/E34, n’est appliquée. Les autres règles de circulation, de type, d’envergure et de virage sont conservées.

Les [règles et leur couverture](docs/OPERATIONS.md) et la [validation des configurations](docs/OPERATIONS-VALIDATION.md) distinguent les interdictions AIP, les sens nominaux rapprochés de la cartographie et les éléments restant à vérifier. Une absence de règle numérisée n’est pas une preuve d’autorisation. Cette version ne prétend pas reproduire toutes les décisions ATC possibles ni les NOTAM en temps réel.


L’import Ramp Agent/vSID accepte une capture JSON documentée dans [INTEGRATION.md](docs/INTEGRATION.md). Il conserve le stand et la piste attribués, puis prépare un point d’attente réel de cette piste. Une piste contraire à la configuration active ou un stand fermé est refusé, sans modifier silencieusement l’affectation du plugin. L’import ne constitue pas une connexion à EuroScope. L’exemple fourni est simulé et soumis aux contrôles de péremption.

## Géographie et couverture

Le fond couvre l’intégralité du périmètre LFPG disponible dans les sources : les quatre pistes physiques avec leurs huit extrémités, terminaux, taxiways, intersections, lignes de parking, stands et points d’attente. Les surfaces AVISO proviennent du dépôt local ; le réseau utilisable conserve les segments OSM et leur géométrie réelle. Les noms/positions de stands sont rapprochés des sources OSM, AVISO et Ramp Agent.

**Couverture cartographique ne signifie pas connectivité complète.** Les sources comportent des stands isolés, des composantes séparées et des écarts géographiques. Les données manquantes sont conservées et documentées ; aucun trait n’est inventé pour rendre artificiellement tous les stands accessibles. Les deux segments de parking qui pénètrent un bâtiment AVISO ont été exclus du routage et conservés en référence. Les accès nommés par piste sont des points d’intersection réels taxiway/piste, pas les seuils de piste ni un choix opérationnel de bretelle.

Consulter [DATA.md](docs/DATA.md), [l’audit de connectivité](public/data/connectivity-report.json) et [la validation géométrique](public/data/geometry-validation.json) pour les nombres exacts et les lacunes géographiques. Les 451 stands reliés géométriquement au réseau principal ne sont pas nécessairement accessibles à tout point dans une configuration donnée. Les matrices opérationnelles indiquent les itinéraires autorisés par le modèle et les refus. Les instantanés originaux et les documents d’exploitation sont conservés dans `data-sources/`.

## Algorithmes et modules

| Module | Responsabilité |
| --- | --- |
| `AirportMap` | Canvas, projection métrique, carte, zoom, sélection et parcours |
| `TaxiGraph` | Adjacence, index et composantes du réseau |
| `OperatingRules` | Contraintes sourcées, contexte est/ouest et arrivée/départ, fermetures, virages et points d’attente |
| `AircraftRestrictions` | Interdictions de sections et de transitions selon type, envergure et conditions du scénario ; conseils distincts des fermetures |
| `RoutePlanner` | A* avec étapes de desserte et d’approche, continuité, courbure cumulée et changement d’axe ; distance physique conservée séparément |
| `AircraftSimulator` | Génération mixte, activation progressive, progression métrique, suivi de l’itinéraire et raccordement après écart |
| `TrafficScheduler` | Répartition déterministe arrivées/départs, heures prévues et espacement des origines réutilisées |
| `PriorityController` | Priorités d’arrivée sur les branches sourcées, attente et reprise sans attribution de clairance |
| `TrafficPredictor` | Occupation temporelle continue des segments et intersections |
| `ConflictDetector` | Détecteur historique sur segments et intersections, conservé pour les tests de référence |
| `PhysicalConflictDetector` | Empreintes orientées selon longueur, envergure et cap, mouvement continu et balayage des virages |
| `SurfaceTracker` | Rapports espacés, rapprochement au graphe, estimation bornée et confirmation des écarts |
| `SurfaceSurveillance` | Frontière entre rapports indépendants, estimation de la carte et flotte fournie aux prédictions |
| `SurveillanceConflictMonitor` | Qualité des prévisions, confirmation des alertes, menace proche et hystérésis |
| `EuroScopeSurfaceAdapter` | Primitives SDK copiées, projection métrique, sessions et rejet des rapports dupliqués ou anciens |
| `RampAgentAdapter` / `VSIDAdapter` | Lecture des champs réels du Plugin Bridge et validation de fraîcheur |

Le moteur `src/core/` est indépendant du DOM. Les coordonnées sont en mètres est/nord autour de 49.0097° N, 2.5479° E ; durées en secondes et vitesses en m/s. Le portage futur vers les structures C++ et le polling ESB existant est décrit dans [INTEGRATION.md](docs/INTEGRATION.md).

Le détecteur actif utilise des **enveloppes physiques orientées** suivant la longueur, l’envergure et le cap du fuselage, distinct du déplacement. Pour A320, A388, B744 et B748, l’empreinte réunit des bandes de fuselage, ailes/moteurs et empennage ; les autres types emploient un rectangle conservateur. Les longueurs connues comprennent les quatre profils vérifiés et 873 entrées du catalogue natif existant. Une longueur inconnue est remplacée par une estimation explicitement signalée, avec une alerte limitée au niveau avertissement.

Les contacts sont recherchés en mouvement continu entre les rapports, avec balayage des rotations aux virages, y compris entre voies physiquement sécantes sans nœud commun dans le graphe. La marge d’activation est de **2 m entre enveloppes**, avec **5 m à la disparition** pour l’hystérésis. L’incertitude de surveillance porte sur le délai et la confiance ; elle ne gonfle pas arbitrairement tous les avions. Ces marges restent des paramètres de simulation, pas des minima ATC. Le délai d’entrée dans la marge et celui de recouvrement des empreintes sont distingués : aucun n’est annoncé comme une heure d’impact certaine. Les appareils arrêtés continuent d’occuper leur empreinte. Le [modèle d’enveloppes](docs/PHYSICAL-ENVELOPES.md) précise les sources, replis, hypothèses d’ancrage et limites du modèle horizontal. L’ancien détecteur 45 m/30 m demeure uniquement une référence de test.

## Vérifier et reproduire

```powershell
npm.cmd test
npm.cmd run validate
npm.cmd run validate:operations
npm.cmd run validate:flows
npm.cmd run validate:route-quality
npm.cmd run validate:aircraft
npm.cmd run validate:traffic
npm.cmd run validate:surveillance
npm.cmd run validate:replays
npm.cmd run build
```

Les tests couvrent A* contre Dijkstra, mouvements exacts sur les segments, sens conditionnels, virages, absence de raccourcis par les stands, points de clairance, fournisseurs externes et scénarios LFPG jusqu’à 100 avions. Ils vérifient aussi la répartition des horaires et des mouvements, l’exclusion des avions programmés des prévisions actives et le suivi après une erreur de roulage. Ils exécutent le bundle de production et ses contrôles dans un DOM simulé. `validate` conserve la validation géométrique indépendante ; `validate:operations` vérifie le moteur contraint dans les deux configurations et les trois mouvements. Résultats : [VALIDATION.md](docs/VALIDATION.md) et [OPERATIONS-VALIDATION.md](docs/OPERATIONS-VALIDATION.md). Les rendus géographiques sont des inspections séparées, pas des captures de navigateur.


`validate:route-quality` vérifie le transit hors des baies de dégivrage, la continuité des axes et les régressions de parcours signalées. Résultats : [ROUTE-QUALITY-VALIDATION.md](docs/ROUTE-QUALITY-VALIDATION.md), [traces avant/après](docs/route-quality-review-routes.json) et [planche de revue](docs/lfpg-route-quality-review.png). Le cas B12 → 26R conserve les sens de référence : la GMC face ouest indique N vers le nord sous G3 ; le trajet demandé vers le sud est donc recherché via F et ses raccordements, sans inverser N.

La [revue des approches D3, T3 et D5](docs/HOLDING-TURN-SOURCES.md) contrôle les cas exacts B08, M09 et B03, avec leurs [traces avant/après](docs/holding-turn-review-routes.json) et leur [planche géographique](docs/lfpg-holding-turn-review.png). Le coût de courbure tient compte des courbes constituées de nombreux petits segments sans les transformer en autant de virages dans le compteur d’embranchements.

`validate:aircraft` contrôle les quatre profils, les deux configurations et les trois mouvements avec le quatrième catalogue utilisé par l’interface. Le rapport [AIRCRAFT-VALIDATION.md](docs/AIRCRAFT-VALIDATION.md) précise les parcours et les effectifs réellement obtenus. Ces scénarios homogènes permettent de vérifier chaque mouvement séparément ; le générateur de l’interface produit un mélange échelonné. Les origines compatibles peuvent être réutilisées lorsque le réseau est limité ; l’espacement de la génération mixte ne constitue pas une validation d’occupation simultanée des stands.

`validate:traffic` contrôle le générateur mixte utilisé dans l’interface : 100 avions par profil et configuration, équilibre arrivées/départs, indicatifs uniques, horaires, rejeu des itinéraires et premières secondes de progression sans fausse alerte hors itinéraire. Les avions encore programmés sont exclus des conflits actifs. Le [rapport de trafic mixte](docs/TRAFFIC-VALIDATION.md) précise la durée simulée et conserve les empreintes des quatre catalogues et du code.

`validate:surveillance` rejoue des parcours LFPG avec des rapports de position, cap et vitesse bruités espacés de cinq secondes, sans transmettre au tracker les identifiants de segment du simulateur. Il contrôle les estimations entre rapports et la confirmation d’une mauvaise branche réelle. Le [rapport de surveillance](docs/SURVEILLANCE-VALIDATION.md) précise les bruits injectés, les durées, les confirmations et les empreintes des sources. Les tests séparés couvrent doublons, rapports anciens, avion arrêté, repoussage, ambiguïtés et alertes de collision stabilisées.

`validate:replays` exploite les vrais rapports EuroScope du dossier local fourni. Créer ou actualiser les caches avec `python scripts/import_euroscope_replays.py --source "CHEMIN_DES_REPLAYS"`, puis lancer la validation. Le [rapport des replays](docs/REPLAY-VALIDATION.md) donne les sessions, cadences, exclusions, écarts aux références inférées et proximités à examiner. Les journaux ne fournissent pas les clairances complètes : ces écarts ne sont pas automatiquement des erreurs de pilote. Les fichiers d’origine restent en lecture seule et les caches anonymes sont ignorés par Git.

Pour reconstruire le jeu de données inclus, Python 3.10+ et Shapely sont nécessaires (uniquement pour les outils, pas pour le simulateur) :

```powershell
python -m pip install -r scripts/requirements-data.txt
python scripts/import_lfpg.py
python scripts/validate_lfpg.py
python scripts/map_operating_directions.py
python scripts/import_operations.py
python scripts/import_route_corridors.py
```

Les options de rafraîchissement réseau sont décrites dans [DATA.md](docs/DATA.md). Ne pas confondre une nouvelle récupération avec la reproduction du même instantané.

## Limites de cette version

- Les règles de type, d’envergure et les virages cartographiés sont appliqués au profil déclaré. Les limites de couverture sont détaillées dans les catalogues. La masse de roulage réelle, les efforts moteurs, les volumes balayés et l’occupation simultanée des ponts ne sont pas déduits du MTOW ou de l’envergure. Les transitions de cap et les vitesses restent simplifiées.
- Aucun modèle de pushback/remorquage, de clairance ATC ou de séparation certifiée. Les pistes sont affichées mais exclues des raccourcis de roulage.
- Les alertes de conflit ne délivrent aucune autorisation et ne résolvent pas l’ensemble des rencontres. Les traversées de piste exigent un déblocage manuel ; le trafic dense peut encore présenter des conflits.
- Aucune connexion temps réel à EuroScope et aucune modification du plugin natif. L’import de capture ne remplace pas le futur transport natif.
- Les sources sont des instantanés de développement, non des données de navigation officielles. Les lacunes de connectivité restent explicites dans les rapports.
- L’interface est compilée et servie localement ; les contrôles visuels interactifs du navigateur n’ont pas pu être exécutés dans cet environnement, qui n’expose aucun navigateur à l’outil de vérification. La géographie et les parcours font l’objet d’un rendu séparé et de contrôles automatiques.

Attribution : © les contributeurs OpenStreetMap, données ODbL 1.0. Voir [DATA.md](docs/DATA.md) pour AVISO, Ramp Agent et leurs notices respectives. Le statut de redistribution de la configuration Ramp Agent doit être clarifié avant une publication du jeu agrégé.
