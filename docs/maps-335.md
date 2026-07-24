# Fonctionnement des maps en 3.3.5 (WotLK)

Synthèse conceptuelle : d'où viennent les données de terrain, comment elles sont découpées, et comment
le monde ouvert, les instances et le phasing s'articulent. (Volontairement indépendant des détails
d'implémentation du code actuel.)

## 1. Vue d'ensemble — trois jeux de données dérivés du client

Le serveur ne lit jamais les données du client directement : trois extracteurs les convertissent en
formats propres au serveur, tous découpés selon la même grille spatiale.

| Données | Outil | Fichiers produits | Rôle serveur |
|---|---|---|---|
| Terrain (ADT/WDT) | `mapextractor` | `maps/MMMXXYY.map` | Hauteur du sol, liquides, area ids, trous |
| Collision (WMO/M2) | `vmap4extractor` + `vmap4assembler` | `vmaps/MMM.vmtree` + `MMM_XX_YY.vmtile` + modèles `.vmo` | Ligne de vue, hauteur sous plafond, intérieur/extérieur |
| Navigation | `mmaps_generator` | `mmaps/MMM.mmap` + `MMMXXYY.mmtile` | Pathfinding des créatures (Recast/Detour) |

`MMM` = map id sur 3 chiffres, `XX`/`YY` = coordonnées de la tuile. Les mmaps sont générés **à partir**
des maps + vmaps (+ modèles de GameObjects) : les trois doivent provenir de la même version du client.

## 2. Données côté client

- **MPQ** : archives contenant tout le contenu du jeu.
- **Map.dbc** : registre des cartes — map id, nom, type (continent, donjon, raid, BG, arène), map
  parent pour les "entrées" (ex. l'entrée d'un donjon pointe vers sa position sur le continent).
- **WDT** (1 par map) : indique quelles tuiles de terrain existent (certaines maps n'ont pas de
  terrain du tout, uniquement un WMO global — ex. la plupart des donjons).
- **ADT** (1 par tuile) : le terrain lui-même — heightmap, textures, liquides, et les *placements*
  de modèles M2 (décor) et WMO (bâtiments).
- **WMO** : bâtiments/structures (un donjon est typiquement un unique WMO "global" sans ADT).
- **M2** : modèles (arbres, rochers…).

En 3.3.5 : 4 continents (0 Azeroth-Est, 1 Kalimdor, 530 Outland, 571 Northrend), plus les maps
instanciables et quelques maps "à part" non instanciables (ex. 609 zone de départ DK).

## 3. Découpage spatial

- Une map est une grille de **64×64 grids** ; un grid fait **533,33 yards** de côté.
- Chaque grid est divisé en **8×8 cells** (~66,7 yd) — la cell est l'unité de visibilité/notification.
- L'origine (0,0) du monde est au **centre** de la map (grid [32,32]) ; les axes vont vers le
  nord-ouest, d'où les conversions avec inversion `(63 - gx)` entre coordonnées "objets" (NGrid)
  et coordonnées "fichiers" (tuiles map/vmap/mmap) — les deux conventions coexistent partout.
- **Chargement paresseux** : un grid (objets + tuile de terrain associée) n'est chargé que lorsqu'un
  joueur ou un objet actif s'en approche ; il est déchargé après un délai d'inactivité
  (configurable, `GridUnload`/`GridCleanUpDelay`).

## 4. `.map` — le terrain

Par tuile : hauteurs (deux grilles V8/V9, éventuellement compressées), liquides (type et hauteur :
eau, lave, slime…), area ids (pour zone/sous-zone), trous (holes). C'est la source de :

- `GetHeight` (sol), `GetWaterLevel`, statut de liquide (nager / marcher au fond / lave qui brûle) ;
- l'area id → AreaTable.dbc → zone, PvP, repos, découverte ;
- la détection de chute à travers le monde.

Sans vmaps, le serveur ne "voit" que ce relief : pas les ponts, grottes ni bâtiments.

## 5. vmaps — la collision

Géométrie réelle des WMO/M2 assemblée par tuile dans un BVH (arbre `.vmtree` par map, spawns par
`.vmtile`). Les modèles (`.vmo`) sont partagés entre maps et référence-comptés. Fournit :

- **ligne de vue** (LoS) — sorts, aggro, évade ;
- **hauteur sous structure** : dans un donjon multi-niveaux, le `.map` seul donnerait l'étage du
  dessus ou rien ; les vmaps donnent le "vrai" sol au-dessus de la tête ;
- **intérieur/extérieur** (montures, Camouflage) et area ids de WMO via WMOAreaTable.dbc ;
- liquides propres aux WMO (ex. eau d'Orgrimmar).

Cas particulier : les donjons sans ADT n'ont qu'une "fausse" tuile vmap (le WMO global est chargé
avec la map entière dès la première tuile).

À côté des vmaps **statiques**, le serveur maintient un arbre **dynamique** (DynamicMapTree) pour la
collision des GameObjects mobiles/destructibles (portes, élévateurs, bateaux, bâtiments de Strand) —
c'est lui qui est filtré par phaseMask.

## 6. mmaps — la navigation

Navmesh Recast/Detour par tuile, généré hors-ligne depuis maps + vmaps. Fournit le pathfinding
(PathGenerator) : chemins qui contournent murs et falaises, off-mesh connections (liaisons
sautées/étroites), coût de l'eau. Sans mmap, une créature "charge en ligne droite" à travers tout.

Point important pour les instances : les **tuiles** de navmesh sont chargées une fois par map id et
partagées par toutes les copies ; chaque copie (instance id) possède seulement son propre objet de
requête (`dtNavMeshQuery`) pour la thread-safety.

## 7. Monde ouvert vs instances

- **Continents** : une seule copie serveur par map id. Grilles chargées/déchargées au fil des
  déplacements. (En 3.3.5 vanilla-TC, ces maps peuvent être découpées en threads de mise à jour,
  mais restent une copie unique.)
- **Maps instanciables** (donjons, raids, BG, arènes — d'après Map.dbc) : le serveur crée **une
  copie par groupe/match** : `InstanceMap` (liée à une sauvegarde d'instance + difficulté) ou
  `BattlegroundMap`. Chaque copie a un **instance id** distinct mais le **même map id**.
- **Toutes les copies partagent les mêmes données disque** : mêmes tuiles .map, vmaps, mmaps. Seuls
  diffèrent l'état dynamique (créatures, GO, scripts, loot, boss down) et le DynamicMapTree.
- **Difficultés** (donjon N/HC ; raid 10/25/10HC/25HC) : même terrain, même map id — seule la copie
  serveur (spawns/scripts) change.

### Parenté des maps côté serveur (le minimum à savoir)

En 3.3.5 il n'existe qu'**une seule relation de parenté** : pour chaque map id instanciable, le
serveur garde un objet "base" (`MapInstanced`, jamais peuplé de joueurs) qui possède les données
terrain partagées et la liste des copies ; chaque copie pointe vers lui (`m_parentMap`). Pour une
map non instanciable, `m_parentMap == this`. Parent et copies ont le même `GetId()`.

Master ajoute une **seconde** parenté, orthogonale : les *chaînes de terrain* (terrain swaps Cata+,
`m_childTerrainMaps` / `m_parentTerrainMap`), où une map peut afficher le terrain d'un autre map id.
Ça n'existe pas en 3.3.5 : lors des ports, `m_parentTerrainMap` ≙ `m_parentMap`, et les boucles sur
`m_childTerrainMaps` disparaissent (d'où la fusion `LoadMap`/`LoadMapImpl`).

## 8. Phasing 3.3.5

- Un `phaseMask` 32 bits par objet/joueur (`PHASEMASK_NORMAL = 1`), modifié par des auras
  (`SPELL_AURA_PHASE`, ex. quêtes DK, Icecrown) ou par script.
- C'est de la **pure visibilité** : deux entités se voient si leurs masques s'intersectent. Créatures
  et GO sont spawnés avec un masque ; un joueur "phasé" voit un autre décor d'objets.
- **Le terrain, les vmaps statiques et les mmaps sont identiques dans toutes les phases.** Seul le
  DynamicMapTree (collision des GO) est interrogé avec le phaseMask. C'est la grande différence avec
  Cata+ où le phasing peut remplacer le terrain lui-même (terrain swap) — la raison pour laquelle
  tout le scaffolding parent/enfant de master est du code mort pour nous.

## 9. Transports

- **Élévateurs** (GO type TRANSPORT) : locaux à une map, mouvement cyclique ; les passagers sont en
  coordonnées relatives au GO ; collision via DynamicMapTree.
- **Bateaux/zeppelins** (GO type MO_TRANSPORT) : suivent un TaxiPath et peuvent **traverser
  plusieurs maps** (ex. Ratchet–Booty Bay) ; le serveur les fait exister sur les deux maps et
  téléporte les passagers avec eux.
