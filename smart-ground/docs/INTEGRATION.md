# Compatibilité Ramp Agent, vSID et futur vSMR

Ce prototype lit des affectations déjà décidées par les plugins. Il n'alloue ni stands ni pistes. L'import JSON est un transport de démonstration local et n'établit aucune connexion active à EuroScope, même si `simulated` vaut `false`.

## Sources vérifiées

Analyse des dépôts officiels le 12 novembre 2026. Les liens sont figés sur les révisions observées, afin de rendre le contrat vérifiable.

| Source | Révision | Éléments analysés |
| --- | --- | --- |
| [EuroscopeRampAgent](https://github.com/AlexisBalzano/EuroscopeRampAgent/tree/d94d6b79aaa0fe5d66790f99864cd2d2a248b357) | `d94d6b79aaa0fe5d66790f99864cd2d2a248b357` | [`src/RampAgent.h`](https://github.com/AlexisBalzano/EuroscopeRampAgent/blob/d94d6b79aaa0fe5d66790f99864cd2d2a248b357/src/RampAgent.h), [`src/RampAgent.cpp`](https://github.com/AlexisBalzano/EuroscopeRampAgent/blob/d94d6b79aaa0fe5d66790f99864cd2d2a248b357/src/RampAgent.cpp), déclaration du fournisseur et publication/effacement des stands |
| [RampAgent-Config](https://github.com/vaccfr/RampAgent-Config/tree/63513c01e3989d6f1f02b3fa366201939be3cb5c) | `63513c01e3989d6f1f02b3fa366201939be3cb5c` | [`airports/LFPG.json`](https://github.com/vaccfr/RampAgent-Config/blob/63513c01e3989d6f1f02b3fa366201939be3cb5c/airports/LFPG.json), version `v1.1.3`, 448 stands |
| [vSID](https://github.com/AlexisBalzano/vSID/tree/311215214599930411118f79a844871e63c36774) | `311215214599930411118f79a844871e63c36774` | [`vSIDPlugin.h`](https://github.com/AlexisBalzano/vSID/blob/311215214599930411118f79a844871e63c36774/vSIDPlugin.h), [`vSIDPlugin.cpp`](https://github.com/AlexisBalzano/vSID/blob/311215214599930411118f79a844871e63c36774/vSIDPlugin.cpp), `RegisterBridgeProvider`, `updateBridgeValues` |
| [vsid-configurations](https://github.com/vaccfr/vsid-configurations/tree/d69919d1c534fa444ae9081befe6aa24f232381c) | `d69919d1c534fa444ae9081befe6aa24f232381c` | [`lfpg.json`](https://github.com/vaccfr/vsid-configurations/blob/d69919d1c534fa444ae9081befe6aa24f232381c/lfpg.json), pistes, intersections et zones nord/sud |

Les sources C++ du dépôt local confirment le contrat : `vSMR/src/integrations/RampAgentBridgeData.hpp`, `RampAgentBridgeClient.cpp`, `VsidBridgeData.hpp`, `VsidBridgeClient.cpp` et `ProxyBridgeSnapshot.hpp`. Aucun de ces fichiers n'est modifié par le prototype.

## Champs réellement publiés

Tous les champs consommés sont des `ESB_T_STR` de portée avion. Les appels ABI et handles ESB restent natifs ; TypeScript reçoit seulement des copies de valeurs.

| Fournisseur/champ qualifié | Schéma observé | Cap UTF-8 | Usage Smart Ground |
| --- | --- | --- | --- |
| `rampagent/stand` | `1.0` | 32 octets | Stand attribué, conservant les zéros et suffixes |
| `rampagent/remark` | `1.0` | 128 octets | Remarque du stand, seulement si un stand existe |
| `vsid/rwy` | `1.1` | 32 octets | Piste publiée par vSID, sans réinterpréter sa logique |
| `vsid/sid` | `1.1` | 32 octets | Information de contexte |
| `vsid/cfl` | `1.1` | 32 octets | Information de contexte, conservée en texte |

Le nom upstream est **`rwy`**, pas `runway`. `VSIDAdapter` expose `runway` dans le résultat normalisé uniquement. Un schéma majeur différent de 1 est incompatible ; les versions mineures restent compatibles par résolution des champs. Champ absent, valeur effacée et fournisseur absent sont des états distincts.

Ramp Agent publie puis efface `stand`/`remark` lorsqu'il perd l'affectation. vSID publie la piste qui correspond à son tag : elle peut provenir d'une sélection ATC ou de sa sélection SID/piste. La valeur `---` signifie qu'aucune piste n'est disponible. Smart Ground consomme ce résultat ; il ne calcule jamais une piste à partir du SID, des pistes actives ou des zones.

Les champs globaux vSID `automode`, `rules`, `areas`, `autoconfig` et les anciens champs Paris ne sont pas nécessaires pour lire une affectation. Aucun contrôle automatique de ces modes n'est ajouté. La configuration LFPG énumère `08R,08L,09R,09L,26L,26R,27L,27R` et des noms d'intersections. Elle ne fournit pas les axes de roulage géographiques. Les zones `north` et `south` sont des surfaces de sélection de procédures, pas des taxiways.

`RampAgent-Config` fournit des points de stands (`Coordinates` = latitude:longitude:rayon), terminaux et règles d'attribution (`Use`, `Code`, `Callsigns`, `Block`, `Priority`, etc.). Seuls les points/noms peuvent alimenter le fond géographique. Les règles restent la responsabilité de Ramp Agent ; un point de stand isolé ne justifie pas d'inventer une bretelle vers le taxiway le plus proche.

## Format d'import local

L'objet `snapshot` reproduit la structure JSON que sérialise déjà `ProxyBridgeSnapshot.hpp`. Les clés des tables de champs avion sont les indicatifs. Les champs globaux éventuels utilisent la clé vide `""`, mais les adaptateurs Smart Ground ne les lisent pas.

L'enveloppe extérieure est **un format Smart Ground**, ajouté pour identifier l'aéroport, la fraîcheur et le caractère simulé. Elle n'est ni un export actuel automatique des plugins ni un nouveau protocole EuroScope.

```json
{
  "format": "smart-ground.bridge.v1",
  "airport": "LFPG",
  "capturedAt": "2026-11-12T18:00:00.000Z",
  "simulated": true,
  "snapshot": {
    "callsigns": ["AFR123"],
    "providers": {
      "rampagent": {
        "major": 1,
        "minor": 0,
        "fields": {
          "stand": {"AFR123": "F21"},
          "remark": {"AFR123": "Demo only"}
        }
      },
      "vsid": {
        "major": 1,
        "minor": 1,
        "fields": {
          "rwy": {"AFR123": "26R"},
          "sid": {"AFR123": "ELCOB1B"},
          "cfl": {"AFR123": "100"}
        }
      }
    }
  }
}
```

`public/data/bridge-example.json` est un exemple figé, explicitement simulé. Son horodatage deviendra périmé ; pour un essai de l'import frais, créer une copie avec l'heure UTC de l'essai en gardant `simulated: true`. Ne jamais rajeunir l'heure d'un véritable export pour masquer sa péremption.

API indépendante du navigateur :

```ts
import { parseBridgeImport } from './src/adapters';
const imported = parseBridgeImport(JSON.parse(fileText), Date.now());
// imported.transport === 'file', jamais 'live'
// imported.assignments: callsign, stand, runway, remark, sid, cfl,
//                       standState, runwayState
```

États d'un champ : `value`, `unset`, `unavailable`, `incompatible`, `stale`, `invalid`. Seul `value` peut fournir une affectation. La durée de validité par défaut est de 15 secondes ; un horodatage supérieur de plus de 5 secondes à l'horloge locale est également inutilisable. L'appelant doit réévaluer avec l'heure courante s'il conserve le résultat affiché. Un fichier ne se met pas à jour tout seul.

Les imports sont limités à LFPG et 4 096 indicatifs normalisés uniques ; versions et tables sont validées. Les caps sont mesurés en octets UTF-8. Les valeurs trop longues ou nulles incorporées sont invalides. Les contrôles ASCII des textes Ramp Agent sont remplacés par des espaces ; les champs vSID rejettent des contrôles internes. La normalisation des stands applique seulement `trim`/majuscules : `A01` et `A1` restent distincts. Aucune approximation ne rattache silencieusement un stand à une autre position.

Une affectation valide reste une référence de plugin. Sa correspondance à un `AirportLocation` du graphe nécessite un nom exact unique ; zéro correspondance, plusieurs correspondances ou `nodeId: null` doivent être signalés dans l'interface. Le routeur ne doit pas inventer un trajet lorsque cette correspondance échoue. Pour les pistes, sélectionner ensuite un point d'accès géographique documenté ; la piste seule ne constitue pas une autorisation d'entrer ou de traverser.

## Portage futur vers vSMR

Le portage doit aussi conserver la distinction entre interdiction AIP et cheminement nominal MANEX : ne pas réintroduire les anciennes flèches AVISO déduites comme des interdictions. Les étapes d’accès d’aire et l’objectif de continuité des axes de `TaxiFlowPolicy` font partie du mode actif. Le futur moteur C++ doit reproduire ces étapes et les comparaisons avant/après, ainsi que les points d’attente ; un simple plus court chemin orienté ne suffit plus.

Le routage opérationnel ajoute une étape après la lecture des affectations : vérifier que le stand est ouvert dans le profil de travaux et que la piste exacte appartient aux départs de la configuration choisie. Le simulateur propose ensuite un point d’attente existant et modifiable sur cette même piste ; il ne remplace pas la piste de vSID par son choix. Les accès PPR sont exclus sans accord préalable simulé explicite. Une affectation contradictoire avec la configuration est signalée, sans bascule silencieuse des pistes en service. Préparer un départ conserve les avions déjà présents, y compris les arrivées, chacun gardant son propre contexte de mouvement.

1. Conserver le polling ESB déjà présent dans vSMR sur le thread EuroScope : résolution des fournisseurs/champs avec types attendus, suivi de révision, invalidation des handles et retrait des valeurs lorsque le fournisseur disparaît. Ne pas faire d'appel ESB depuis un worker de calcul.
2. Fournir au moteur Smart Ground une copie immuable des `VsmrRampAgent::AircraftData` et `VsmrVsid::AircraftData`, plus indicatif, aéroport et horodatage de capture. Ne transmettre aucun pointeur ABI interprocessus.
3. Résoudre les noms exacts vers le graphe LFPG et exposer les références manquantes. Éviter de dupliquer une seconde connexion Ramp Agent/vSID ou leurs politiques d'affectation.
4. Porter les structures simples du graphe, A*, progression sur arêtes et prédiction vers des valeurs C++ (`std::vector`, IDs, distances en mètres, secondes). Ajouter des comparaisons de résultats sur les mêmes jeux de données et scénarios déterministes avant d'activer une intégration réelle.
5. Afficher des propositions et alertes seulement. La décision ATC, les autorisations, l'entrée piste et les restrictions opérationnelles restent à l'opérateur et aux systèmes existants.

Le prototype ne charge aucun DLL, ne modifie aucun fichier de configuration des plugins et n'émet aucune commande `.vsid`, attribution Ramp Agent ou autorisation ATC.

## Validation des adaptateurs

`node --import tsx --test tests/adapters.test.ts` vérifie la forme JSON actuelle du proxy, la propriété des valeurs par fournisseur, l'effacement stand/remarque, l'absence de piste non déduite d'un SID, les fournisseurs/champs manquants, le schéma majeur incompatible, la péremption et l'horodatage futur, les caps UTF-8, les contrôles, les pistes LFPG, `---`, les dates et les indicatifs mal formés. Ces tests utilisent des valeurs synthétiques et n'établissent aucune intégration native.
