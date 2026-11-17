# Contrat de surveillance pour le futur branchement EuroScope

Cette note rapproche les données de surveillance de Smart Ground du SDK effectivement présent dans le dépôt. La cadence d'environ **5 secondes** vient du scénario décrit par l'utilisateur ; le SDK inspecté ne garantit pas un intervalle fixe. Le suivi doit utiliser les dates réelles des rapports, accepter les retards et distinguer un nouveau rapport d'un nouveau dessin de l'écran.

Le branchement natif de cette chaîne n'est pas réalisé par cette note. Les sources C++ existantes restent inchangées. Le simulateur sert à éprouver le suivi et les alertes avant leur portage ou leur exposition au plugin.

## Données réellement disponibles

Référence locale : [EuroScopePlugIn.h](../../lib/include/EuroScopePlugIn.h), SHA-256 `641f8b31cd2416b80263826e36e283184ccf8c1c34b3cb9b3a1d4192d53eef52`. Les références ci-dessous désignent cette version embarquée, sans supposer qu'une autre version du SDK possède exactement le même contrat.

| Donnée | API et référence | Sens et précaution |
| --- | --- | --- |
| Position | `CRadarTarget::GetPosition()` ligne 2412 ; `CRadarTargetPositionData::GetPosition()` ligne 610 ; `CPosition::m_Latitude/m_Longitude` lignes 281–282 | Copier les coordonnées, puis les projeter dans le même repère métrique que le graphe. Ne pas calculer les distances de sécurité en pixels. |
| Âge du rapport | `CRadarTargetPositionData::GetReceivedTime()` ligne 601 | **Nombre entier de secondes écoulées depuis la réception**. Ce n'est ni un horodatage Unix, ni une date de mesure à bord, ni un numéro de rapport. |
| Vitesse transmise | `GetReportedGS()` ligne 667 | Vitesse sol transmise par le client pilote. vSMR l'utilise en nœuds : [Rimcas.cpp](../../vSMR/src/safety/Rimcas.cpp), ligne 213, la convertit par `0.514444` en m/s. |
| Vitesse de repli | `CRadarTarget::GetGS()` ligne 2362 | Le SDK prend d'abord la vitesse transmise ; il utilise la vitesse calculée **si la vitesse transmise vaut zéro**. Ce n'est donc pas une seconde mesure indépendante à moyenner avec `GetReportedGS()`. Un arrêt réel peut coexister avec une vitesse calculée encore non nulle. |
| Orientation de l'avion | `GetReportedHeading()` et `GetReportedHeadingTrueNorth()` lignes 676–677 | Le commentaire SDK précise qu'il s'agit du **cap, pas de la trajectoire**. Utiliser la variante explicitement référencée au nord vrai pour comparer au graphe. Zéro degré est un cap valide ; aucun code d'absence particulier n'est documenté ici. |
| Direction calculée du déplacement | `CRadarTarget::GetTrackHeading()` ligne 2353 | Direction calculée en degrés. La référence vrai/magnétique n'est pas explicitée par le commentaire inspecté : conserver cette provenance. Pour le suivi géométrique, calculer de préférence la direction à partir de deux positions suffisamment espacées. |
| Historique | `GetPreviousPosition(current)` ligne 2428 | Rapports antérieurs disponibles pendant la requête. Copier aussi leur âge si on veut les utiliser pour estimer vitesse, virage ou arrêt. `CFlightPlan::GetFPTrackPosition()` n'est pas une entrée valide pour parcourir cet historique. |
| Identité | `GetCallsign()` ligne 2321 ; `GetSystemID()` ligne 2330 | Le SDK indique que l'identifiant système est dérivé de l'indicatif. Ajouter une génération de session lors d'une disparition/reconnexion ; l'identifiant seul ne prouve pas qu'il s'agit du même suivi continu. |
| Altitudes | `GetPressureAltitude()` ligne 648 ; `GetFlightLevel()` ligne 657 | Les deux commentaires SDK donnent des **pieds**, y compris `GetFlightLevel()`. Ne pas interpréter ce dernier comme un nombre de centaines de pieds dans l'adaptateur. |
| Type et contexte | Plan de vol : `GetAircraftFPType()`, origine, destination, corrélation, état sol ; capture [RadarScene.cpp](../../vSMR/src/scene/RadarScene.cpp), lignes 548–574 | Conserver le type ICAO pour les restrictions de gabarit. Un état attribué par un contrôleur ou une corrélation n'est pas une mesure physique de mouvement ou une clairance déductible automatiquement. |
| Réponses radar | `GetRadarFlags()` ligne 706 | Drapeaux primaire/secondaire C/secondaire S. Ces drapeaux ne fournissent pas une précision métrique ou une covariance. Ne pas en inventer une prétendument issue du SDK. |

`CPosition::DirectionTo()` (ligne 325) renvoie explicitement une **direction magnétique utilisant la déclinaison du secteur actif**. Ce résultat ne doit pas être passé comme un cap vrai sans conversion. Au repère local est/nord, une direction vraie peut être obtenue avec `atan2(deltaEst, deltaNord)`, normalisée en degrés de 0 à 360. La direction du déplacement devient peu informative lorsque la distance entre rapports est comparable au bruit de position ; elle ne suffit alors pas à déclarer un mauvais sens.

## Ce que la scène native capture aujourd'hui

[RadarScene.cpp](../../vSMR/src/scene/RadarScene.cpp), lignes 488–544, copie déjà position, vitesses, position précédente, historique, altitudes, squawk, cap transmis, direction calculée et cap vrai dans [VsmrScene::Target](../../vSMR/src/scene/RadarScene.hpp), lignes 130–192. Les âges des rapports et leur séquence indépendante ne sont pas conservés dans cette structure.

`RadarScene::frameId` et `captureTick` décrivent une capture de scène ; `captureTick` reçoit `GetTickCount()` à la ligne 332. Un rafraîchissement de carte peut donc changer cet instant sans aucune nouvelle position reçue. Reconstruire `maintenant - GetReceivedTime()` à chaque dessin n'apporte pas de nouveau témoignage et comporte la quantification entière de l'âge.

Le champ `Target::airborne` utilise actuellement un classement destiné aux tags, fondé sur des seuils de vitesse de 40/50 kt dans [TargetRoleLogic.hpp](../../vSMR/src/scene/TargetRoleLogic.hpp). Ce n'est pas un bit sol/vol indépendant transmis par un capteur. Le futur filtrage de surveillance sol devra tenir compte du contexte et de l'altitude sans transformer ce classement d'affichage en certitude physique.

Les polygones de retour primaire créés dans [RadarScreen.FlightDataAndCommands.cpp](../../vSMR/src/radar/RadarScreen.FlightDataAndCommands.cpp), lignes 30–125, contiennent un aléa graphique. Ils ne constituent ni des positions supplémentaires ni une estimation du bruit de surveillance. Le repoussage peut aussi faire diverger fortement cap du fuselage et direction du déplacement ; le RIMCAS existant distingue déjà ces deux grandeurs dans [Rimcas.cpp](../../vSMR/src/safety/Rimcas.cpp), lignes 521–538.

## Enveloppe à fournir au moteur

L'adaptateur natif devra copier les valeurs primitives à l'arrivée d'un rapport, puis fournir au moteur une identité de suivi, un numéro de rapport monotone par cible, l'instant monotone de réception, l'âge fourni par EuroScope, les coordonnées, la vitesse sol et le cap vrai disponibles, ainsi que leur provenance. Une date de mesure réelle ne doit être ajoutée que si un fournisseur la transmet effectivement. La précision et la latence non connues restent des paramètres explicites du modèle, pas des données SDK inventées.

Le SDK propose `OnRadarTargetPositionUpdate()` sur les interfaces de plugin et d'écran (lignes 3002 et 3853). Le dépôt possède déjà le callback d'écran à [RadarScreen.FlightDataAndCommands.cpp](../../vSMR/src/radar/RadarScreen.FlightDataAndCommands.cpp), ligne 6. Choisir **un seul point d'ingestion central**, puis distribuer ses instantanés aux écrans : plusieurs vues d'une même cible ne doivent pas compter comme plusieurs rapports confirmant un écart.

Les références SDK de position, plan de vol et historique ne doivent pas être conservées entre callbacks ou transférées à un worker : les remarques du SDK limitent leur validité au bloc de requête. Le moteur portable devra recevoir des copies possédées de nombres, chaînes et tableaux. Il pourra être rejoué sur un journal de rapports sans appeler le SDK ni modifier les affectations ATC.

La frontière concrète est maintenant [EuroScopeSurfaceAdapter.ts](../src/adapters/EuroScopeSurfaceAdapter.ts). Elle reçoit des primitives `aircraftId`, `sessionId`, `sequence`, `receivedAt`, `latitude`, `longitude`, et les champs optionnels `reportedGroundSpeedKnots`, `effectiveGroundSpeedKnots`, `headingTrueDegrees`. Son origine et son échelle sont configurables ; par défaut elles reproduisent exactement la projection LFPG du graphe. Elle n'appelle pas le SDK et ne crée aucune connexion EuroScope.

L'appelant ouvre une génération avec `beginSession(id, sessionId)`, puis appelle `adapt(report, consumedAt)`. Le résultat distingue un rapport accepté, avec son `SurfaceSample`, d'un refus motivé. Un doublon, une séquence ancienne, un instant de réception non croissant, un rapport provenant d'une autre génération ou une valeur invalide ne consomme pas le prochain numéro valide. Rappeler `beginSession()` avec la même génération est sans effet ; remplacer explicitement la génération nécessite aussi de retirer l'ancien suivi du `SurfaceTracker` et des mémoires d'alerte. `endSession()` et `clear()` ferment les suivis concernés.

Pour un rapport accepté, `sample.observedAt` et `sample.receivedAt` restent égaux à l'instant monotone du callback fourni par l'appelant. `consumedAt` ne rafraîchit jamais la mesure. Cette convention décrit un instant local de réception, pas une date de mesure à bord ; le champ entier SDK `GetReceivedTime()` ne doit jamais être passé directement comme cette valeur. Le calcul de vitesse de repli n'est retenu que lorsque la vitesse transmise vaut explicitement zéro et sa provenance est marquée `effective-fallback`. Si la vitesse transmise manque, la valeur effective seule n'est pas présentée comme une mesure indépendante : le tracker peut estimer le déplacement à partir des positions suivantes.

Les reconstructions entre rapports servent à estimer une position probable et un intervalle d'incertitude. Elles ne créent pas de nouvelles observations pour confirmer un écart d'itinéraire. Le parcours attribué guide une hypothèse de mouvement, mais une observation persistante sur une autre voie doit rester observable. Une ambiguïté à une intersection ou entre voies proches doit diminuer la confiance plutôt que déclencher immédiatement un recalcul.

## Scénarios de validation reproductibles

- Rapports espacés de 5 s avec un bruit latéral déterministe, puis cadence variable de 4 à 7 s ; roulage normal et virages franchissant plusieurs petits segments du graphe entre deux rapports.
- Avion arrêté avec fluctuations du cap et positions voisines ; aucun mauvais sens confirmé à partir du cap seul.
- Point proche de deux axes parallèles ou d'un carrefour ; maintien d'une hypothèse ambiguë, puis résolution par les rapports suivants et le déplacement observé.
- Mauvaise bifurcation et mauvais sens persistants ; confirmation par plusieurs rapports indépendants, sans compter les images interpolées comme témoignages.
- Rapport dupliqué, reçu hors ordre, retour après déconnexion, saut de position invraisemblable et rapport manquant ; rejet ou qualité dégradée explicite, sans extrapolation illimitée.
- Face-à-face et croisement réels, puis jitter autour d'une limite d'alerte ; menace proche crédible traitée rapidement, alerte lointaine incertaine confirmée et extinction avec hystérésis.
- Arrêt devant un point d'attente, récupération et reprise manuelles ; aucune prédiction nominale ne suppose une clairance future, et une position réellement reçue au-delà du point n'est pas effacée par une remise artificielle sur la route.

Les amplitudes de bruit, durées de confirmation et budgets d'extrapolation sont des paramètres de validation. Ils devront être confrontés à des captures de rapports EuroScope avant tout réglage final dans vSMR.
