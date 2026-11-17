# Restrictions aeronefs et envergure a LFPG

Le catalogue [lfpg-aircraft-restrictions.json](../public/data/lfpg-aircraft-restrictions.json) complete les contraintes d'exploitation et les cheminements MANEX. Il ne remplace pas les fermetures, les sens, les portails d'aire, les exclusions de degivrage ou les points de clairance. Aucune geometrie du graphe n'est ajoutee ni deplacee.

La version verifiee contient **70 regles de voie**, **65 virages diriges interdits** (61 generaux et 4 conditionnels au-dessus de 36 m) et **une consigne non bloquante** d'oversteering pour l'A380. Les regles de voie touchent 1 462 aretes source distinctes, avec des conditions qui ne s'appliquent pas toutes au meme avion. L'empreinte du catalogue est `913a066d2bfde160f7fdf716942c58a8ac5530931a033d74f0033fae8ec63882`.

La [planche de revue](lfpg-aircraft-restrictions-review.png) compare l'effet du catalogue sur A320, B744, B748 et A388, sans restrictions de travaux, avec les axes centraux et les baies SW libres. Elle montre les restrictions sur la geometrie source; ce n'est pas une capture de navigateur ni un test de rayon de virage physique.

## Sources et interpretation

- [SIA LFPG AD2, edition AIRAC du 29 octobre 2026](https://www.sia.aviation-civile.gouv.fr/media/dvd/eAIP_29_OCT_2026/FRANCE/AIRAC-2026-10-29/html/eAIP/FR-AD-2.LFPG-fr-FR.html), paragraphes 20.3.3.2.2, 20.3.3.2.3, 20.3.3.2.4 et 20.3.3.3. Ce texte donne les seuils d'envergure, les clauses conditionnelles, les restrictions de virage et les consignes A380/B748.
- [GMC07 face Est](../data-sources/aircraft/lfpg-gmc-07-20261029.pdf) et [GMC08 face Ouest](../data-sources/aircraft/lfpg-gmc-08-20261029.pdf), cartes du 11 juin 2026, dans la meme edition AIRAC. Leurs empreintes respectives sont `edf0cb9a74610005b618d15d2992f96209c36a0bec6874a7c1d992000b0aef0d` et `6218af9ddf540e1bfba4919eafe174320b4f0b3806aae90b74dfb94deac28b42`.
- La [carte des virages dangereux](../data-sources/aircraft/viragedangereux_1025.png) et le [schema des lignes axiales](../data-sources/aircraft/LFPG.20_croquis_virages_interdits.png) sont extraits du meme AIP. Les [notes des virages](TURN-RESTRICTIONS-SOURCES.md) conservent leurs groupes de noeuds et d'aretes ainsi que les sens verifies.
- Les instantanes OSM et les etiquettes AVISO servent uniquement a identifier la geometrie existante. Les noms homonymes, les raccordements et les limites de section sont verifies separement; une simple proximite a une etiquette ne ferme pas un ensemble de voies.

Les cartes orange nomment A380, B748, A124 et C5. Le texte AD2 renvoie aussi a ces cartes pour les voies interdites aux avions d'envergure **superieure a 65 m**. Le catalogue applique donc ces deux conditions independamment : le type explicitement cite et le depassement de 65 m pour tout autre type. Le B747-400 n'est pas assimile au B747-8. Les chaines C5A, C5B, C5C et C5M sont acceptees comme alias utilisateurs de la famille C5 de la legende, sans pretendre constituer une liste de designateurs ICAO distincts.

Les largeurs de bande degagee d'obstacles (38 m, 40 m, 41,7 m, etc.) et la liste des voies de largeur inferieure a 23 m ne sont **pas** converties en limites fictives d'envergure. Une largeur de chaussee ou une distance a un obstacle n'est pas une envergure maximale publiee.

## Seuils inclusifs et modes conditionnels

| Clause publiee | Emprise du modele | Application |
| --- | --- | --- |
| G1, maximum 33 m | Chaine source G1W `59465748`, sous l'etiquette actuelle G1 | Fermee si envergure >33 m; 33 m reste autorise par cette seule clause. |
| C2, E1, E2, E10, E11, E12, E13, G2, P4, P5, BM1, maximum 36 m | Chaines source identifiees; E12 taxiway de Juliett est distinct du stand E12 au terminal 2E | Fermees seulement au-dessus de 36 m. |
| A4, G3, G4, P3, TB2, axes orange et bleu, maximum 36 m | Empreinte du taxilane concerne | Activee par le mode `colored-dual-axis`; l'axe central reste le mode par defaut. |
| EM23, maximum 52 m | Chaines `180375120` et `237392456` | Fermee seulement au-dessus de 52 m. |
| A3, maximum 61 m | Chaine C ouverte `172859840`, sans fermer A exterieur | Fermee seulement au-dessus de 61 m. |
| EM5, maximum 36 m depuis EN81 | Partie sud a partir de la jonction reelle du parking EN81 | La partie nord n'est pas etendue arbitrairement. Les contraintes independantes de tractage restent actives. |
| FN4, maximum 36 m et clause de masse | Chaine FN4 `59465739` | Seuil d'envergure applique. La masse reelle de roulage >80 t reste explicite dans les limites du catalogue; elle n'est pas deduite d'un MTOW. |
| WT, maximum 36 m | Tronc commun `229282487:3`, entre `2379042571` et `2379042572`, environ 9,7 m | Les autres bras de raccordement T/W3/W4 ne sont pas fermes en bloc. |
| T entre T1 et T2, maximum 36 m si un avion attend sur SW1-SW4 | Chemin source T entre les deux vraies jonctions T1/T2 | Active seulement avec `southwest-deicing-occupied`, valable toute l'annee. |

Les deux flags orange/bleu et SW occupees sont des etats de scenario explicites, faux par defaut. Le graphe ne comporte qu'une ligne centrale : le mode orange/bleu applique sa restriction d'envergure mais ne cree pas deux traces paralleles decalees et ne prouve pas qu'une circulation simultanee est possible.

## Restrictions des grands gabarits

L'orange GMC est rapproche des voies publiees A3/A4, G3/G4/TB2, G2, E1-E4, E9-E13, P1-P5, de l'aire H et des dessertes de Juliett, de la partie ouest d'E jusqu'au carrefour B/GE1, de M et de B jusqu'a BD1, des raccordements BM/MD explicitement colores, de Y2/Z5 et des portions sud indiquees sur les cartes. Les identites et les limites de chaque groupe figurent dans le champ `mapping` du catalogue.

Les limites geographiques suivantes sont verifiees explicitement :

- **E** : la chaine ouest `222948626` est coloree jusqu'a B/GE1; la chaine orientale `170450475` n'est pas fermee par cette regle.
- **B cote FedEx** : le segment ferme s'arrete a la vraie jonction B/BD1 `1690796776`; la partie est de B demeure distincte.
- **MD2** : seule la partie entre B et M est concernee par ce groupe; la portion vers D/D5 n'est pas confondue avec elle.
- **E12** : seul le taxiway de Juliett `152634782` est selectionne; le parking homonyme `1531265018` au terminal 2E reste hors de cette regle.
- **S** : la partie ouest de V3 vers V4 comprend `222951672:0` et `515164111`; la partie V5-S6 comprend `515164108` et `515164109:0`. S1/S2/S3 a l'ouest et S6-S9 a l'est restent blancs et ne sont pas fermes en bloc.
- **V2 et V3** : les bretelles principales blanches restent disponibles. Le groupe orange V3 ne vise que le raccord complementaire source `158959145`, qui rejoint la piste sans jonction intermediaire. La diagonale V3 principale `158959143` reste hors de ce groupe.
- **U/C cargo en vert** : l'interdiction propre a l'A380 porte sur les branches occidentales U/C et UC1-UC5, jusqu'aux jonctions ouest reelles d'UT1 (`1721703882` sur U et `42395887` sur C). UT1 et les frontages U/C a l'est restent hors de cette fermeture par type. Le B748 n'herite pas de la restriction verte.

Les voies autorisees au tractage dans le texte AIP ne deviennent pas utilisables en roulage autonome. Le simulateur ne cree ni mission de tractage, ni extinction moteur, ni clairance de pont a partir du seul profil d'aeronef.

## Virages et consignes

Les 65 triples diriges sont detaillees dans [TURN-RESTRICTIONS-SOURCES.md](TURN-RESTRICTIONS-SOURCES.md). Les restrictions de gabarit comprennent le mouvement pont A vers A3 et son inverse au raccordement sud-est verifie, ainsi que A vers DA1, strictement au-dessus de 36 m. Le sens inverse DA1 vers A n'est pas invente comme interdit par la meme phrase.

L'enonce AIP interdisant les virages sans ligne axiale s'applique partout, mais les documents fournis ne constituent pas une preuve de marquage pour chaque noeud du graphe. Les croisements et exemples dangereux numerises, la continuite aux intersections synthetiques et l'interdiction des demi-tours restent des controles distincts. La topologie OSM seule ne prouve pas qu'un virage est peint ou eclaire. L'acces ouest d'A3 n'est pas assimile au pont sans preuve geographique; la portee minimale verifiee est conservee.

L'oversteering demande a l'A380 dans TA3 vers R cap a l'est est une **consigne non bloquante**, liee aux vrais arcs. Elle apparait comme advisory lorsqu'un parcours l'emprunte; elle n'est pas transformee en interdiction.

## Reproduction et controle

```powershell
python scripts/map_aircraft_turns.py
python scripts/import_aircraft_restrictions.py
python scripts/render_aircraft_restrictions_review.py
npm.cmd exec tsx -- --test tests/aircraft-restriction-regressions.test.ts
```

Le generateur verifie les identifiants, les sources et l'empreinte immuable du graphe. Le rendu verifie aussi les empreintes des fichiers source. Les dix tests de regression calculent leurs attentes depuis les identites et les directions physiques relevees dans les documents, sans recopier les regles generees comme oracle.

Les restrictions liees aux travaux SUP151/26 et SUP188/26 sont exclues a la demande de l'utilisateur : les fermetures temporaires de voies et de postes et les limites temporaires code C sur E15/E34 ne sont plus appliquees. Les documents restent conserves dans les sources pour la provenance; les autres restrictions sont inchangees.

Le catalogue d'exploitation historique contient encore des mentions de restrictions aeronef exclues lors de sa premiere implementation. Elles decrivent cette couche ancienne et ne signifient plus que le simulateur ignore le gabarit : le present catalogue independant la complete. La couverture de la masse reelle, du souffle moteur, des marges a l'obstacle, de l'occupation simultanee des ponts et de la compatibilite physique des stands n'est pas deduite de l'envergure.
