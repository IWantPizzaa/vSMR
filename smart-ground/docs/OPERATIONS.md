# LFPG : règles de roulage et configurations

Le mode courant combine les contraintes décrites ici avec les [cheminements MANEX et les axes principaux](CORRIDORS.md). Les anciennes projections AVISO ne sont plus traitées comme des interdictions générales ; la [validation du mode courant](FLOW-VALIDATION.md) remplace les résultats du seul routage directionnel pour évaluer la cohérence des parcours.

Le moteur opérationnel sépare les restrictions des géométries. Le fichier `public/data/lfpg-operations.json` porte les sources, sens, accès aux pistes, points d'attente, priorités, travaux datés et limites de couverture. Le graphe géographique d'origine n'est pas modifié.

**L'équivalence exhaustive avec toutes les clairances et tous les roulages réels n'est pas démontrée.** Les sens connus et les interdictions prouvées sont appliqués, les sections non vérifiées sont signalées. Une flèche AVISO non datée ou un carrefour OSM ne constitue pas, à lui seul, une preuve de toute la procédure AIP courante. Aucune règle liée au type, à l'envergure ou à la masse de l'avion n'est appliquée dans cette version, conformément à la demande.

## Sources officielles et version

Consultation le **12 novembre 2026**, édition eAIP du **29 octobre 2026** :

| Source | Portée | Version du document |
| --- | --- | --- |
| [SIA LFPG AD 2](https://www.sia.aviation-civile.gouv.fr/media/dvd/eAIP_29_OCT_2026/FRANCE/AIRAC-2026-10-29/html/eAIP/FR-AD-2.LFPG-fr-FR.html) | AD 2.13, AD 2.20 : accès, restrictions, priorités, clairances | Édition 29 OCT 2026 |
| [GMC 01 face EST](https://www.sia.aviation-civile.gouv.fr/media/dvd/eAIP_29_OCT_2026/FRANCE/AIRAC-2026-10-29/html/eAIP/Cartes/LFPG/AD_2_LFPG_GMC_01.pdf) | Cheminements ARR et DEP | 11 JUN 2026 |
| [GMC 02 face OUEST](https://www.sia.aviation-civile.gouv.fr/media/dvd/eAIP_29_OCT_2026/FRANCE/AIRAC-2026-10-29/html/eAIP/Cartes/LFPG/AD_2_LFPG_GMC_02.pdf) | Cheminements ARR et DEP | 11 JUN 2026 |
| [ADC 02](https://www.sia.aviation-civile.gouv.fr/media/dvd/eAIP_29_OCT_2026/FRANCE/AIRAC-2026-10-29/html/eAIP/Cartes/LFPG/AD_2_LFPG_ADC_02.pdf) | Points CAT I/CAT III, barres d'arrêt et entrées interdites | 02 OCT 2025 |
| [SUP 151/26](https://www.sia.aviation-civile.gouv.fr/media/store/documents/file/l/f/lf_sup_a_2026_151_fr.pdf) | Travaux A/B en plusieurs phases | AIRAC 06 AUG 2026 |
| [SUP 188/26](https://www.sia.aviation-civile.gouv.fr/media/store/documents/file/l/f/lf_sup_a_2026_188_fr.pdf) | Travaux P3/TE3 et fermetures de stands | AIRAC 01 OCT 2026 |

Les PDF, le HTML et leurs extractions sont conservés dans `data-sources/operations/`. Les empreintes SHA-256 figurent dans le manifeste. Les trois cartes principales et les annexes des travaux ont été rendues et inspectées visuellement ; les polices encodées de certaines cartes ne permettent pas une extraction textuelle fiable. Le manifeste distingue la date de l'édition eAIP de celle imprimée sur chaque carte.

Les flèches complémentaires AVISO, leur appariement borné aux segments et les exceptions ARR/DEP sont documentés dans [DIRECTIONS.md](DIRECTIONS.md). Elles restent explicitement `authoritative: false`. Les règles AIP sur une même arête prennent la priorité sur cette couche supplémentaire.

## Configurations et mouvements

Le modèle standard distingue départ, arrivée et transfert. Le sens d'une voie peut dépendre du mouvement : par exemple E en configuration EST, ou R/T en configuration OUEST, ne doit pas recevoir une seule règle commune contradictoire pour ARR et DEP.

| Configuration | Départs préférentiels | Arrivées préférentielles |
| --- | --- | --- |
| Face EST | 09R / 08L | 09L / 08R |
| Face OUEST | 27L / 26R | 27R / 26L |

La préférence intérieure/départ et extérieure/arrivée vient d'AD 2.20.3.2.1. L'interface utilise ce profil standard : il ne faut pas en déduire que tout usage exceptionnel de l'autre piste serait interdit dans la réalité. Les exceptions nécessitant une configuration et une clairance particulières ne sont pas toutes modélisées.

Les départs se terminent désormais à des **points d'attente réels** : ils ne vont pas jusqu'au centre de la piste. Les arrivées commencent après le dégagement de la piste extérieure et avant la traversée de la piste intérieure. L'atterrissage, la décélération sur piste et le choix performant d'une sortie ne sont pas simulés.

Les 28 accès livrés sont des alias de nœuds OSM existants de type `holding_position:type=runway`, du bon côté de la piste et sur le connecteur indiqué. Aucun point d'attente n'est déplacé arbitrairement vers une voie proche. Les accès D6, Q2, Q3, K1 et K2 ne disposent pas de point CAT I source correctement rattaché et restent dans les lacunes, plutôt que de réutiliser un point CAT III ou une intersection de piste à leur place.

AD 2.13 réserve les plus grandes TORA à une demande préalable : D1/09R, Q6/27L, T1 et T2/08L, R1/26R sont marqués `pprRequired`. Le moteur les masque sans approbation explicite de scénario. Il conserve la condition de préavis de 30 minutes dans les raisons publiées. L'interdiction supplémentaire de R1 pendant l'exploitation du dégivrage 26R est documentée ; cet état n'est pas reçu en temps réel.

## Contraintes appliquées

Les règles permanentes comprennent **247 règles d'arêtes**. Ce nombre est celui des enregistrements, pas celui des règles AIP distinctes ni celui des taxiways entièrement vérifiés.

| Domaine | Traduction dans le modèle |
| --- | --- |
| Entrées interdites vers une piste | Z3/Z5/Z6, Y1–Y6, W1–W6, V3–V6 orientés en sortie uniquement ; références AD 2.20.3.2.1.7 et .3.3.2.3 |
| MD2 entre B et M | Sens vers la piste interdit, limité à la portion publiée au sud de B ; la portion B–D n'est pas étendue arbitrairement |
| EM3–EM8 | Fermés au roulage autonome car réservés au tractage moteurs arrêtés ; aucun tractage n'est simulé |
| Entrée / traversée de piste | 60 points de contrôle directionnels avant piste sur des positions source réelles ; poursuite uniquement sur décision manuelle |
| Priorités | 15 approches de jonction possèdent une branche entrante à céder et les arêtes entrantes prioritaires exactement incidentes |
| Raccourcis par les stands | Lignes de parking réservées aux chaînes d'origine/destination, pas au transit intermédiaire |
| Demi-tours | Reprise immédiate de la même arête interdite |
| Croisements synthétiques | Virages entre segments sources non prouvés rejetés ; poursuivre le même segment reste possible |

Pour Z3, Z5 et Z6, la portion touchant la piste était laissée sans nom dans OSM. Les continuations source `223198024`, `157174701` et `157174628` sont intégrées à la restriction de leur sortie respective. Il s'agit d'arêtes existantes, sans raccordement ajouté. WT n'a pas d'axe explicitement identifiable dans ce jeu : cette restriction reste répertoriée comme non appariée ; les entrées vers la piste par W2/W3 sont déjà interdites. D7/Q1/Y1A et Z4 apparaissent fermés sur les sources et sont absents des axes routables inclus.

Les priorités publiées sont Q envers Q2/Q3/Q4 et T envers T9/T10/T11 face EST ; D envers D4/D5/D6 et T envers T4/T5/T6 face OUEST. Leur application n'est possible qu'aux fusions effectivement identifiées dans le graphe. Le respect d'une priorité ne donne aucune autorisation d'entrer sur une piste et ne remplace pas les points d'attente.

L'arrêt anticipé à 45 m d'une jonction et la marge temporelle de 12 secondes du contrôleur de priorité sont des paramètres de simulation, pas des normes SIA. Un avion déjà immobilisé devant une clairance de piste ne réserve pas une rencontre future qu'il ne peut pas encore rejoindre.

Le modèle des virages reste conservateur. L'AIP interdit les virages dont l'axe n'est pas matérialisé. Le rejet des jonctions artificielles et du transit par les stands évite plusieurs raccourcis invalides, mais ne prouve pas tous les marquages de virage aux nœuds déjà présents dans OSM. Les interdictions de cheminement A↔A3 entre satellites du terminal 1 restent donc partiellement couvertes, sans prétendre que toute la géométrie de marquage est connue.

## Travaux : profils datés séparés

Les profils ne constituent pas un flux NOTAM. Ils doivent être sélectionnés dans l'interface avec leur période et leur état de confirmation.

**SUP 188/26**, du 1er octobre au 8 décembre 2026 : le calendrier publié ferme P3 au travers E09–E44 et TE3. Le profil ferme 52 segments géographiques correspondants et les stands explicitement concernés. Il est activé par défaut pour le scénario daté du **12 novembre 2026**, sans affirmer une validation NOTAM en direct ni une actualisation à la date de l'ordinateur. Les consignes de repoussage vers l'ouest restent des notes : le pushback n'est pas simulé. Les restrictions code C sont exclues à la demande de l'utilisateur.

**SUP 151/26, phase 2**, prévue du 1er octobre 2026 au 4 janvier 2027 : les phases sont activées par NOTAM ; ce profil reste donc désactivé par défaut. Il ferme 17 segments de B entre DA1 et BD9 et de l'approche A entre BD9 et X05, ainsi que les stands publiés. Deux courbes distinctes de BD9 existent dans OSM : le modèle utilise la limite ouest représentée par l'annexe, pas un chemin passant par tout l'anneau A. Des assertions bornent géographiquement et métriquement cette emprise. La desserte exclusive de X05 sur le tronçon résiduel A, ainsi que les instructions de push-pull, restent des conditions non entièrement simulées.

## Nomenclature : écarts détectés et corrigés localement

Les axes du véritable P3 près du terminal 2E sont encore nommés `E0 Axe central`, `E0 Axe orange` ou `E0 Axe bleu` dans l'instantané OSM. Le ref OSM `P3` se trouve environ 850 mètres plus à l'est et ne correspond pas au P3 de la carte AVISO/AIP.

`edgeNameOverrides` corrige uniquement les axes corroborés par la position AVISO et les cartes officielles : P3 sur les voies source `179460847`, `277202903`, `277202904`, `1525559583`, `1525559584`, `1075534451` ; TE3 sur `179458124` et `179458113`, anciennement RP5 ou sans nom. Le ref P3 éloigné n'est ni utilisé pour fermer le mauvais secteur, ni renommé sans preuve de son nom actuel. Des assertions vérifient que les fermetures SUP 188 sont bien dans le secteur réel de 2E.

Ces correspondances changent les noms d'affichage du graphe de travail, pas la géométrie ni les fichiers des sources. L'écart X05/X5 est utilisé pour repérer une limite géographique de travaux ; les identifiants d'affectation des stands ne sont pas fusionnés silencieusement.

Les fermetures de stands retirent aussi les alias d'une même position : nœud identique, ou nom numérique équivalent confirmé à moins de 25 m. Ainsi X3 ne peut pas contourner la fermeture de X03, ni W6/W7 celle de W07. Cette correspondance locale de fermeture ne modifie pas les règles de résolution exacte des affectations Ramp Agent.

## Ce qui reste explicite

Le registre `ruleLedger` qualifie chaque domaine : appliqué, partiel, conseil, non apparié ou exclu à la demande. `coverage.completeIRLEquivalence` vaut toujours `false` tant que les limites suivantes subsistent :

- Les axes OSM sans règle de direction vérifiée restent identifiés, sans inventer leur sens. Le routage sur une section non couverte ne prouve pas sa conformité AIP.
- Les clairances de repoussage, GREEN/RED PUSH/PULL, traction, gestion d'aire et procédures de dégivrage ne sont pas complètement simulées.
- Aucune bascule LVP, état réel de barre d'arrêt ou RWSL, activation NOTAM ou accord PPR réel n'est reçu.
- Les marques peintes de tous les virages et le réseau de desserte du terminal 1 ne sont pas intégralement décrits par la topologie OSM.
- Les règles de gabarit, type, masse et leur sous-ensemble moteur restent exclues. Les recommandations d'oversteering ne sont pas converties en faux rayons de virage.

Les sources complémentaires examinées comprennent SUP 169/26 (essai du balisage REL sur S3, sans changement d'itinéraire) et SUP 187/26 (régime moteur dépendant du gabarit, hors périmètre). Le relevé de SUP n'est pas une garantie d'exhaustivité des NOTAM.

## Reproduction

À partir des instantanés inclus, depuis `smart-ground/` :

```powershell
python scripts/map_operating_directions.py
python scripts/import_operations.py
npm.cmd test
npm.cmd run validate
```

L'import d'exploitation utilise seulement la bibliothèque standard Python. Les rendus PDF de revue utilisent PyMuPDF dans l'environnement de développement ; ils ne sont nécessaires ni pour lancer l'application, ni pour reconstruire le manifeste depuis les instantanés. Les sources SIA portent leur attribution © SIA ; leurs copies sont des références du prototype local, sans présumer des conditions d'une redistribution publique.
