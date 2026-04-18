# vSMR - Fonctionnement CDM auto

Le plugin surveille les appareils au sol qui correspondent a l'aeroport actif dans vSMR. Lorsqu'un avion devient eligible, il n'est pas rappele immediatement : son callsign est d'abord memorise avec une heure de detection, puis le plugin attend le delai que t'as configure avant d'envisager un envoi. Ce delai sert donc de temporisation, pour eviter de relancer trop vite un avion qui vient juste d'apparaitre dans l'etat surveille, et pour eviter de spam les gars qui deco reco 10x a des gates differentes. Si tu regles ce delai a `0` (en minutes), le rappel devient immediat.

Quand l'echeance est atteinte, l'avion est simplement ajoute a une file d'envoi. Juste avant l'injection reelle du `.msg`, son eligibilite est recalculee a partir de son etat. Le message n'est envoye que si le vol correspond toujours a l'aeroport actif, qu'il est encore considere comme au sol, qu'il est toujours dans un etat NSTS, et qu'aucune clairance datalink n'a deja ete envoyee a ce callsign. Si entre-temps le pilote a quitte cet etat, a commence a rouler, a change de statut ou a deja ete traite autrement -> pas de message.

Le texte envoye est lu depuis l'alias `.cdm` dans `Alias/alias.txt`. Les formes `.cdm <texte>`, `.cdm .msg <texte>` et `.cdm .msg $aircraft <texte>` sont acceptees. Anyways le plugin ne garde que le contenu utile du message. Si l'alias est absent ou mal forme, vSMR affiche un avertissement dans EuroScope et n'envoie rien.

La seule maniere simple que j'ai trouve pour eviter le spam c'est de rajouter un cooldown -> la duree minimale a attendre avant de pouvoir renvoyer un rappel au meme callsign apres un envoi reussi (pt a changer avec le CID idk). Tant que la duree n'est pas ecoulee, le callsign reste bloque pour eviter le spam, que le declenchement vienne de la commande manuelle ou du mode automatique. + un appareil deja present dans la file d'envoi n'est pas ajoute une deuxieme fois. A noter en revanche que, dans l'etat actuel du code, cet historique est vide si le vol se deconnecte. Donc si t'as un mode auto avec delai de 1min, que le mec se co, attend 1 min, recois le message, se deco, et se reco, attend 1 min, il se reprend un message (apres, de la a dire que c'est du spam...).

| Commande | Ce que ca fait | Exemple |
| --- | --- | --- |
| `.smr cdm` | Lance immediatement un controle des appareils actuellement eligibles a l'aeroport actif. La commande tente de les placer dans la file d'envoi et affiche ensuite un resume avec le nombre d'appareils verifies, ajoutes, deja notifies, deja en file, deja exclus car deja traites en datalink, ou en echec. | `.smr cdm` |
| `.smr cdm auto` ou `.smr cdm auto status` | Affiche l'etat du mode automatique ainsi que le delai actuellement configure avant rappel. | `.smr cdm auto status` |
| `.smr cdm auto on` ou `.smr cdm auto enable` | Active le mode automatique en conservant le delai deja enregistre. A partir de la, chaque avion nouvellement detecte comme eligible commence son temps d'attente avant rappel. | `.smr cdm auto on` |
| `.smr cdm auto off` ou `.smr cdm auto disable` | Desactive le mode automatique et vide le suivi temporel des appareils en cours de surveillance. | `.smr cdm auto off` |
| `.smr cdm auto <minutes>` | Active le mode automatique et definit le delai d'attente avant rappel pour un appareil nouvellement detecte. Avec `0`, le rappel part sans temporisation. | `.smr cdm auto 1` |
| `.smr cdm cooldown` ou `.smr cdm cooldown status` | Affiche la duree de cooldown actuellement appliquee entre deux rappels envoyes au meme callsign. | `.smr cdm cooldown` |
| `.smr cdm cooldown <minutes>` | Modifie la duree de cooldown anti-spam entre deux rappels vers un meme appareil. 60 par defaut. | `.smr cdm cooldown 60` |
