# Validation des restrictions aéronef LFPG

Commande : `npm.cmd run validate:aircraft`. Le calcul emploie les mêmes quatre catalogues et règles que l’interface, sans restrictions liées aux travaux. Les prédictions ne délivrent aucune clairance de traversée.

Les 2400 parcours contrôlés respectent les contraintes d’envergure, de type et de virage lors de la recherche et du rejeu. Les 576 000 positions suivent exactement leurs arêtes ; les 800 traversées physiques possèdent un point d’attente source antérieur adapté. Aucun parcours généré ne traverse les voies intérieures des baies de dégivrage exclues.

Chaque scénario contient 100 avions compatibles. Les origines sont d’abord essayées une fois, puis les origines compatibles peuvent être réutilisées si le type ne dispose pas de 100 stands distincts. Changer le profil de planification ne modifie pas celui du trafic déjà créé. Aucun profil de travaux n’est appliqué. Les contextes conditionnels de dégivrage et d’axes colorés sont désactivés dans ces scénarios, et vérifiés séparément par les tests de seuil et de scénario.

| Type | Envergure | Configuration | Mouvement | Avions | Origines distinctes | Génération ms |
| --- | ---: | --- | --- | ---: | ---: | ---: |
| A320 | 35.8 m | east | departure | 100 | 100 | 751.9 |
| A320 | 35.8 m | east | arrival | 100 | 6 | 2123.4 |
| A320 | 35.8 m | east | transfer | 100 | 100 | 1095 |
| A320 | 35.8 m | west | departure | 100 | 100 | 730.8 |
| A320 | 35.8 m | west | arrival | 100 | 4 | 2296.5 |
| A320 | 35.8 m | west | transfer | 100 | 100 | 1137.5 |
| A388 | 79.75 m | east | departure | 100 | 83 | 367.4 |
| A388 | 79.75 m | east | arrival | 100 | 6 | 1417 |
| A388 | 79.75 m | east | transfer | 100 | 52 | 2094.6 |
| A388 | 79.75 m | west | departure | 100 | 83 | 510.1 |
| A388 | 79.75 m | west | arrival | 100 | 4 | 1108.2 |
| A388 | 79.75 m | west | transfer | 100 | 52 | 2125.3 |
| B744 | 64.92 m | east | departure | 100 | 100 | 709.9 |
| B744 | 64.92 m | east | arrival | 100 | 6 | 1412.9 |
| B744 | 64.92 m | east | transfer | 100 | 100 | 1110.5 |
| B744 | 64.92 m | west | departure | 100 | 100 | 858.2 |
| B744 | 64.92 m | west | arrival | 100 | 4 | 1388.9 |
| B744 | 64.92 m | west | transfer | 100 | 100 | 1116.4 |
| B748 | 68.4 m | east | departure | 100 | 100 | 453.2 |
| B748 | 68.4 m | east | arrival | 100 | 6 | 1285.8 |
| B748 | 68.4 m | east | transfer | 100 | 68 | 1579.4 |
| B748 | 68.4 m | west | departure | 100 | 100 | 587.5 |
| B748 | 68.4 m | west | arrival | 100 | 4 | 1494.8 |
| B748 | 68.4 m | west | transfer | 100 | 68 | 1650.2 |

Les durées mesurent le code Node.js local et excluent le rendu Canvas. D’autres contrôles peuvent s’exécuter en parallèle ; ce n’est pas un benchmark isolé.

## Reproduction

Graine : 20261113 ; Node.js : v24.20.0. Résultats détaillés : [aircraft-validation-results.json](aircraft-validation-results.json).

- `public/data/lfpg.json` : `4597cfcb70d555ca801427a378d7f8fca297e55677d3062faed0c1718ed5b9c5`
- `public/data/lfpg-operations.json` : `d881f3494335470dfa11a0fe371f99bc7f2a772c2bca7e7a1785f74d7b603736`
- `public/data/lfpg-corridors.json` : `9228e0c4e9293426c2318ea032ce5ac07fc331595201b8c6eeaa6916cdf8def3`
- `public/data/lfpg-aircraft-restrictions.json` : `913a066d2bfde160f7fdf716942c58a8ac5530931a033d74f0033fae8ec63882`

Les tests indépendants comparent les virages conditionnels à un Dijkstra dirigé et vérifient les vrais identifiants des jonctions transcrites des cartes. Ils contrôlent aussi les bornes strictes ou inclusives, les profils falsifiés, les scénarios activés et les advisories d’oversteering. Les cartes restent des instantanés documentés : ce contrôle ne certifie pas l’intégralité des NOTAM ni toutes les décisions tactiques ATC.
