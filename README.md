# vSMR - Guide CDM automatique

Ce README explique uniquement le fonctionnement de l'envoi automatique de message CDM et des commandes associees. Le module lit le texte du rappel depuis l'alias `.cdm`, surveille les avions au sol qui correspondent a l'aeroport actif, puis prepare des envois prives via la commande EuroScope `.msg`.

Le systeme integre une logique anti-spam. Un appareil deja notifie reste bloque pendant la duree de cooldown, un appareil deja en file d'attente n'est pas ajoute une deuxieme fois, et un appareil deja traite par envoi de clairance datalink n'est plus cible par le rappel CDM. Cette logique protege la frequence et evite les doublons meme quand la commande manuelle et le mode automatique sont utilises en meme temps.

Le contenu du message est extrait de `Alias/alias.txt` a partir de la ligne `.cdm`. Le plugin accepte une forme simple `.cdm <texte>`, ainsi que les formes `.cdm .msg <texte>` et `.cdm .msg $aircraft <texte>`. Si l'alias est absent ou invalide, un avertissement est affiche dans EuroScope.

| Commande | Fonctionnement | Exemple |
| --- | --- | --- |
| `.smr cdm` | Lance un controle immediat des appareils eligibles, alimente la file d'attente et affiche un resume du traitement (queued, deja notifie, deja en file, deja traite, echec). | `.smr cdm` |
| `.smr cdm auto` ou `.smr cdm auto status` | Affiche l'etat du mode automatique et le delai courant avant envoi du rappel. | `.smr cdm auto status` |
| `.smr cdm auto on` ou `.smr cdm auto enable` | Active le mode automatique avec le delai deja configure. | `.smr cdm auto on` |
| `.smr cdm auto off` ou `.smr cdm auto disable` | Desactive le mode automatique et reinitialise le suivi temporel des appareils surveilles. | `.smr cdm auto off` |
| `.smr cdm auto <minutes>` | Active le mode automatique et fixe le delai d'attente avant rappel pour un appareil nouvellement detecte. `0` signifie envoi immediat des eligibles. | `.smr cdm auto 5` |
| `.smr cdm cooldown` ou `.smr cdm cooldown status` | Affiche la valeur de cooldown anti-spam appliquee entre deux rappels d'un meme callsign. | `.smr cdm cooldown` |
| `.smr cdm cooldown <minutes>` | Definit le cooldown anti-spam entre deux envois vers le meme appareil. | `.smr cdm cooldown 60` |

Le flux d'envoi utilise la commande `.msg` d'EuroScope, pas le telex Hoppie, afin de conserver le comportement attendu des messages prives vers les avions sans modifier les fonctions CPDLC existantes.
