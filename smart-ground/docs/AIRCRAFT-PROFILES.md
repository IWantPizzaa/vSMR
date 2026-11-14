# Profils de gabarit du simulateur

Le profil associe un type ICAO déclaré à une envergure en mètres. Il est conservé avec chaque parcours et chaque avion. Le choix dans le panneau de planification s’applique au prochain parcours et au générateur ; il ne modifie pas les avions déjà présents. Sélectionner un avion rétablit son profil pour les calques.

| Profil proposé | Envergure par défaut | Base documentaire |
| --- | ---: | --- |
| A320 avec sharklets | 35,80 m | EASA A.064, issue 62 du 26 juin 2026, page PDF 37. La variante sans modification est donnée à 34,10 m. |
| A380-800, A388 | 79,75 m | Airbus ACP, §2-2-0, figure des dimensions, page PDF 29, feuille datée du 1er novembre 2020. |
| B747-400, B744 | 64,92 m | Boeing D6-58326-1, rév. F de décembre 2024, §2.2.1, page PDF 31 : maximum à masse brute maximale et plein carburant. Le dessin donne aussi 64,44 m en position de référence. |
| B747-8, B748 | 68,40 m | Boeing D6-58326-3, rév. E de janvier 2026, §2.2.1, page PDF 15. |

Les valeurs par défaut sont explicites et prudentes. Un profil personnalisé peut déclarer une variante documentée : par exemple A320 à 34,10 m ou B744 à 64,44 m. Une envergure inférieure au minimum connu d’un de ces types est refusée. Un profil B747 sans variante est ambigu : choisir B744 ou B748. Le code A380 est normalisé en A388.

Les autres codes ICAO déclarés, par exemple B77W, utilisent l’envergure saisie. Ils ne reçoivent pas automatiquement celle d’un avion de la même famille. UNKNOWN/CUSTOM/ZZZZ restent non identifiés : union prudente des restrictions de type, avertissement et exclusion du générateur automatique. Un profil ne constitue pas une affectation de stand ni une preuve de capacité de stationnement.

Sources officielles consultées : [EASA A.064](https://www.easa.europa.eu/en/downloads/16507/en), [ACP Airbus A380](https://www.airbus.com/sites/g/files/jlcbta136/files/2021-11/Airbus-Aircraft-AC-A380.pdf), [ACAP Boeing 747-400](https://www.boeing.com/content/dam/boeing/v2/airports/acaps/747-400_Rev_F.pdf), [ACAP Boeing 747-8](https://www.boeing.com/content/dam/boeing/v2/airports/acaps/747-8_Rev_E.pdf). Les PDF téléchargés, leurs empreintes, les pages de dimensions rendues et les valeurs relevées sont conservés dans [aircraft-profiles](../data-sources/aircraft-profiles/sources.json).

Ces profils alimentent les [restrictions LFPG](AIRCRAFT-RESTRICTIONS.md). La masse de roulage réelle et les volumes balayés ne sont pas déduits de l’envergure ou du MTOW. Les limites géométriques et de séparation du simulateur restent celles de ses modules de mouvement et de conflit.
