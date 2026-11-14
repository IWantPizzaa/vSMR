# Virages publies : sources et portee

Le registre [turn-restrictions.json](../data-sources/aircraft/turn-restrictions.json) ajoute **65 transitions dirigees** : 61 generales et quatre conditionnelles au depassement de 36 m. Les voies et leurs courbes ne sont pas fermees globalement par ce registre.

Les deux figures sources proviennent de l'edition AIP du 29 octobre 2026, lue le 13 novembre 2026 :

- [Carte des virages interdits les plus dangereux](../data-sources/aircraft/viragedangereux_1025.png), image inline de AD2.20.3.3.2.4.3. Elle est distincte de GMC03, qui traite notamment les axes lateraux de roulage.
- [Exemples de lignes axiales courbes](../data-sources/aircraft/LFPG.20_croquis_virages_interdits.png), AD2.20.3.3.2.4.2. La presence d'une connexion OSM ne prouve pas qu'un changement de voie y est marque.

Le texte [AD2](../data-sources/operations/lfpg-ad2-20261029.html), section 20.3.3.2.2, donne les limites de 36 m pour les mouvements entre le pont A et A3, ainsi que depuis A vers DA1. Une envergure exactement egale a 36 m ne depasse pas cette limite.

| Groupe | Transitions dirigees | Portee physique |
| --- | ---: | --- |
| MD2 / D | 4 | Croisement du MD2 nord avec D ; continuation vers D5 distincte. |
| MD1 / B | 8 | Croisement source `intersection:27`. |
| MD2 / B | 8 | Croisement source `intersection:29`. |
| MD2 / M | 4 | Jonction sud `osm:1690901700` ; le court troncon porte le nom BM5 dans OSM. |
| MD1 / BM6 | 8 | Croisement `osm:1690902025`. |
| BD7 / BD8 | 8 | Croisement `osm:1690795982`. |
| A / B, cote occidental et oriental | 4 | Branches en retour de cap ; les deux raccordements tangentiels sont conserves. |
| Y4 vers QD1 | 1 | Branche entrante depuis Y4 uniquement ; QD1 est nomme Q dans les donnees OSM. |
| FN2 / F | 4 | Changement direct de voie ; E/FN2 reste distinct. |
| FN3 / F | 12 | Deux croisements sources rapproches ; G3/F reste distinct. |
| Pont A / A3, envergure >36 m | 2 | Raccordement sud-est A `112642878:57` / A3 `172859840:109`, dans les deux sens. |
| A vers DA1, envergure >36 m | 2 | Deux branches de A vers DA1 ; la restriction n'est pas etendue au sens inverse. |

Le raccordement sud-ouest de A3 n'est pas assimile au pont sur la seule base du nom A. Sa fermeture aux envergures de 36 a 61 m aurait etendu la clause sans preuve de portee. La limite generale A3 de 61 m appartient au catalogue des voies, separement des virages.

Le conseil d'oversteering A380 dans TA3 vers R cap est est conserve comme **conseil**, sur la courbe source et sa sortie vers R est. Il ne devient pas une interdiction du raccordement.

Le catalogue ne pretend pas inventorier chaque absence de marquage au sol : il numerise les carrefours explicitement illustres. Les controles existants des croisements synthetiques et des demi-tours restent appliques independamment. Les triples sont verifies contre les noeuds et aretes d'origine ; aucune geometrie n'est ajoutee.

Reproduction : `python scripts/map_aircraft_turns.py`, puis compilation du catalogue par `python scripts/import_aircraft_restrictions.py`. Les tests [aircraft-restriction-regressions.test.ts](../tests/aircraft-restriction-regressions.test.ts) fixent leurs attentes depuis les identifiants sources et les clauses, sans recopier les conditions generees pour fabriquer les resultats attendus.
