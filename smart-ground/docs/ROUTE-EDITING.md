# Attribution d'un cheminement par déplacement de carrefour

Le parcours calculé initialement reste conservé dans `Aircraft.standardRoute`. `Aircraft.assignedRoute` est la référence actuelle du suivi : elle commence sur le parcours standard, puis devient une copie immuable de la route explicitement attribuée par l'opérateur. `Route.assignment` distingue `standard` et `controller`, conserve une révision et les carrefours imposés dans leur ordre.

## API du moteur

- `previewAssignedRoute(id, { anchorNodeIndex, targetNodeId })` calcule un candidat sans modifier l'avion. L'indice désigne un nœud du parcours actif encore à parcourir ; le nouveau point doit être une intersection existante de taxiways.
- `editAssignedRoute(id, request)` applique le candidat, conserve la position, le cap et la distance déjà parcourue, puis maintient l'avion actif pour reprise explicite. Un mouvement programmé conserve son heure et reste programmé.
- `undoLastRouteEdit(id)` restaure l'affectation précédente lorsque le trajet déjà parcouru et le segment engagé sont toujours identiques. Si l'avion a emprunté la nouvelle branche, l'annulation est refusée : elle ne le replace pas sur un autre taxiway.

Les résultats sont discriminés : `{ ok: true, route, waypoints }` ou `{ ok: false, reason }`. Plusieurs déplacements conservent les points déjà imposés ; déplacer un point imposé remplace cette contrainte.

## Contraintes conservées

Le calcul joint les points imposés dans une recherche continue dont l'état comprend la voie d'entrée, les étapes restantes et la phase de desserte du parking. Les virages aux raccordements entre étapes ne sont donc pas réinitialisés. Le préfixe déjà parcouru et l'arête actuellement engagée restent fixes.

Un cheminement attribué peut remplacer les portails et étapes du modèle nominal. Il conserve les sens des sections, les fermetures générales, les interdictions de virage, les limites de type et d'envergure, les points d'attente, l'exclusion du transit par les baies de dégivrage et l'interdiction des raccourcis par les lignes de stands. Les extrémités, le mouvement et la configuration restent ceux de l'avion.

La mention `controller` seule n'autorise pas un parcours fourni arbitrairement : le planificateur conserve les géométries produites et vérifiées par cette recherche. Les copies immuables autorisées restent rejouables ; changer leur géométrie ou leurs contraintes imposées invalide cette autorisation.

Une modification conserve seulement les autorisations correspondant à des points effectivement dépassés sur le préfixe inchangé. Une autorisation donnée mais non encore franchie doit être renouvelée après modification. L'attribution d'un parcours et la reprise du roulage ne délivrent aucune autorisation de traversée.

La récupération après écart conserve la nouvelle affectation, ses carrefours encore à parcourir et sa révision. Le parcours standard d'origine sert uniquement de comparaison. Le raccordement reste soumis au même rejeu des contraintes.

## Vérification

`tests/route-editing.test.ts` couvre les prévisualisations, plusieurs déplacements, le remplacement de contraintes, les annulations, les virages interdits aux raccordements, un profil A380 sur une voie étroite, les points d'attente, les mouvements programmés et la récupération après modification. Les tests existants de conformité continuent de s'appliquer.

Cas réel contrôlé : face est, M09 vers le point T3 de 08L. Le déplacement du nœud `osm:2318941305` vers l'intersection voisine `osm:8968901466`, distante d'environ 27 m, produit un cheminement différent de 1 446,9 m au lieu de 1 437,9 m. La position de départ, le profil, la destination et la référence standard sont conservés ; le parcours attribué passe le rejeu complet des contraintes applicables.
