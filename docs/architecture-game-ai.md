# Analyse de l'architecture `src/server/game/` et recommandations pour une IA de compagnons intelligente

> Rapport d'analyse — Projet ISC (Immersive SoloCraft), fork TrinityCore 3.3.5
> Date : 2026-07-11

---

## 1. Vue d'ensemble de l'architecture actuelle

`src/server/game/` (~561 fichiers C++) est le cœur gameplay compilé dans le
worldserver. C'est une architecture TrinityCore classique, héritée de MaNGOS,
organisée en modules par domaine :

| Module | Rôle |
|---|---|
| `Entities/` | Hiérarchie d'objets : `Object` → `WorldObject` → `Unit` → `Creature`/`Player` |
| `AI/` | Hiérarchie d'IA des créatures (détaillée en §2) |
| `Movement/` | `MotionMaster` + générateurs de mouvement, splines |
| `Maps/`, `Grids/` | Partitionnement spatial (grilles 533m × cellules), chargement dynamique |
| `Combat/` | `ThreatManager`, `CombatManager` |
| `Spells/` | Système de sorts complet (cast, auras, effets) |
| `Scripting/` | `ScriptMgr` : ~130 hooks virtuels pour les scripts statiques |
| `Server/` | Sessions, opcodes, packets (protocole client 3.3.5) |
| `World/` | Boucle principale `World::Update` |

### 1.1 Patterns utilisés

**Boucle de jeu à tick fixe (~50–100 ms).**
`World::Update` → `MapManager::Update` → `Map::Update` → update de chaque
objet actif → `UpdateAI(diff)`. Le `MapUpdater`
([MapManager.cpp:55](src/server/game/Maps/MapManager.cpp:55)) est un pool de
threads qui parallélise **par carte** : une map = un thread au maximum. À
l'intérieur d'une map, tout est strictement séquentiel et mono-thread.

**Hiérarchie d'héritage + dispatch virtuel pour l'IA.**
`UnitAI` → `CreatureAI` → (`ScriptedAI`, `SmartAI`, `CombatAI`, `PetAI`,
`GuardAI`, `TotemAI`, `ReactorAI`, `PassiveAI`…). Le comportement est défini
en surchargeant des callbacks événementiels
([CreatureAI.h](src/server/game/AI/CreatureAI.h) : `JustEngagedWith`,
`JustDied`, `MoveInLineOfSight`, `SpellHit`, `DamageTaken`, etc.) plus un
`UpdateAI(diff)` appelé à chaque tick.

**Factory + Registry pour la sélection d'IA.**
`CreatureAIFactory`/`CreatureAIRegistry` (pattern `FactoryHolder` +
`ObjectRegistry`) ; `CreatureAISelector` choisit l'IA d'une créature via son
`ScriptName` en base, sinon par un score de priorité statique (`Permitions` :
IDLE < REACTIVE < PROACTIVE < …).

**IA data-driven : SmartScripts.**
`SmartAI`/`SmartScript` (~9 400 lignes) interprète la table SQL
`smart_scripts` : des triplets **Événement → Cible → Action** (ex. « à 20 %
de vie → self → cast Enrage »). C'est essentiellement une machine à états
événementielle plate, configurée en données. Puissant pour du contenu
scripté, mais sans mémoire, sans planification, sans arbitrage entre
comportements concurrents.

**Stack de générateurs de mouvement.**
`MotionMaster` maintient une pile priorisée de `MovementGenerator`
(Chase, Follow, Point, Waypoint, Flee, Formation, Home, Random…). Le
pathfinding s'appuie sur **recastnavigation** (Detour, navmesh « mmaps »).

**Observer/hooks pour le scripting.**
`ScriptMgr` expose ~130 points d'extension virtuels ; les scripts de contenu
(`src/server/scripts/`) s'enregistrent statiquement (`-DSCRIPTS=static`).

**Timers : `EventMap`, `TaskScheduler`, `EventProcessor`.**
Trois mécanismes coexistants de planification de tâches temporisées,
utilisés massivement dans les scripts de boss.

### 1.2 Bibliothèques (dep/)

- **Boost ≥ 1.90** — Asio (réseau), conteneurs, filesystem
- **recastnavigation** — navmesh + pathfinding (Detour)
- **fmt**, **g3dlite** (maths 3D), **SFMT** (RNG), **utf8cpp**
- **jemalloc** (allocateur, Linux), **efsw** (watch fichiers), **argon2**
- MySQL client, OpenSSL, zlib/bzip2, libmpq (lecture des données client)

### 1.3 Forces et limites vis-à-vis de l'objectif ISC

**Forces à préserver :**
- Le pipeline *perception → événement → réaction* (grilles, visibilité,
  ThreatManager) est mature et déjà optimisé.
- `MotionMaster` + mmaps fournissent un socle mouvement/pathfinding solide.
- Le découplage IA/entité (une `CreatureAI` possédée par la `Creature`) rend
  l'IA remplaçable sans toucher au reste — c'est le point d'insertion idéal.
- Beaucoup de marge CPU : le serveur est dimensionné pour des milliers de
  joueurs ; en solo, on peut dépenser 100× plus de budget CPU **par NPC**.

**Limites pour des compagnons intelligents :**
1. **Réactif, jamais délibératif.** Toute l'IA existante répond à des
   événements ; aucune structure ne représente un *but* ni un *plan*
   (« finir ce donjon », « protéger le joueur pendant qu'il loot »).
2. **Pas d'arbitrage.** Quand plusieurs comportements sont valides
   (soigner ? fuir ? interrompre ? DPS ?), rien ne les met en concurrence :
   c'est du premier-arrivé dans l'ordre du code ou des `smart_scripts`.
3. **Pas de mémoire ni d'état partagé.** Aucun blackboard : un NPC ne se
   souvient de rien au-delà de sa threat list, et deux compagnons ne peuvent
   pas coordonner (« je tank, tu soignes »).
4. **SmartScripts plafonne vite.** Exprimer une rotation de classe ou une
   coordination de groupe en lignes SQL événement/action devient
   inmaintenable (c'est précisément pourquoi NPCBots et playerbots ont écrit
   leur propre moteur à côté).
5. **`UpdateAI(diff)` à chaque tick pour chaque créature active** : le modèle
   « tout le monde pense à chaque tick » gaspille le budget quand une seule
   poignée de NPC (les compagnons) a besoin de réflexion profonde.

---

## 2. État de l'art : quels patterns pour des compagnons intelligents ?

Synthèse des recherches (sources en fin de document).

### 2.1 Les quatre grandes familles

| Pattern | Principe | Forces | Faiblesses | Exemples connus |
|---|---|---|---|---|
| **FSM / HFSM** | États + transitions explicites | Simple, prévisible, débogable | Explosion combinatoire, rigide | IA TrinityCore actuelle (de fait) |
| **Behavior Tree (BT)** | Arbre hiérarchique Selector/Sequence/Decorator, ré-évalué chaque tick | Modulaire, réutilisable, standard industriel, outillable | Réactif ; l'adaptabilité reste bornée par la structure de l'arbre | Halo 2+, Unreal Engine, la majorité des AAA |
| **Utility AI** | Chaque action candidate reçoit un *score* via des courbes de réponse ; la meilleure gagne | Comportement émergent et naturel, arbitrage continu, très data-driven, ajout d'une action = local | Moins prévisible, tuning des courbes nécessaire | Les Sims, Guild Wars 2 (heal/support), IAUS de Dave Mark |
| **Planners (GOAP / HTN)** | L'agent cherche une *séquence* d'actions menant d'un état-monde courant à un but | Vraie délibération, plans multi-étapes, s'adapte à des situations non prévues | Coût CPU du planning, débogage plus dur, besoin d'un modèle du monde propre | GOAP : F.E.A.R. ; HTN : Killzone 2+, **Horizon Zero Dawn** (planner + macros d'actions, rôles distribués aux groupes) |

Constat récurrent dans l'industrie : **les équipes qui livrent mixent les
approches** ; le pattern pur est l'exception. La recherche récente (ex.
GOBT, 2023) va explicitement vers des BT hybridés avec de l'utility et du
goal-oriented.

### 2.2 Ce que font les projets bots WoW existants (leçon directe)

- **NPCBots (trickerer, TrinityCore 3.3.5)** — implémente `bot_ai` héritant
  de `CreatureAI`, plus ~30 classes `bot_<classe>_ai` codées à la main
  (rotation, buffs, positionnement) dans `src/server/game/AI/NpcBots/`.
  → Preuve que **l'insertion par `CreatureAI` fonctionne** sans casser le
  moteur, mais le comportement 100 % codé en dur est coûteux à maintenir et
  peu « intelligent » (pas de but, pas d'adaptation).
- **mod-playerbots (ike3 → liyunfan1223/AzerothCore)** — architecture en
  couches **Strategy / Trigger / Action / Value** : des *stratégies*
  activables composent des paires trigger→action pondérées par une
  **relevance** (score d'utilité simplifié), avec un contexte partagé
  (valeurs calculées à la demande = proto-blackboard) et un **throttling
  d'activité** (les bots loin du joueur pensent moins souvent).
  → C'est déjà un hybride utility-léger + event-driven, et c'est l'approche
  la plus proche des besoins ISC.

### 2.3 Et les LLM ?

La littérature 2024-2026 converge vers un cadre mémoire / raisonnement /
E-S : le LLM (ou SLM local fine-tuné) sert au **dialogue, à la personnalité
et à la mémoire épisodique**, tandis que le comportement temps réel
(combat, déplacement) reste piloté par BT/utility/planner classiques —
la latence et le coût d'inférence interdisent le LLM dans la boucle de
combat. Pour ISC (RPG solo axé lore), c'est une brique *optionnelle de
couche narrative*, pas un remplacement du moteur comportemental.

---

## 3. Recommandation d'architecture pour ISC

### 3.1 Choix du pattern : hybride « Utility + HTN léger » sur socle événementiel existant

Pour des **compagnons de groupe dans un RPG solo**, le besoin réel est :

- arbitrage continu entre actions concurrentes (heal/DPS/interrupt/kite) → **Utility**
- comportements séquentiels multi-étapes et coordination de rôles
  (« pull → CC → focus → loot ») → **HTN léger** (macros de plans, comme
  Horizon Zero Dawn, plutôt qu'un GOAP à recherche d'état complet)
- réactivité aux événements du moteur → **conserver les callbacks
  `CreatureAI` existants** comme couche de perception

Concrètement, une pile en trois couches par compagnon :

```
┌─────────────────────────────────────────────────┐
│ Couche 3 — Buts & plans (HTN léger, ~1 Hz)      │  « nettoyer la salle »,
│   décompose un but en tâches assignées aux rôles │  « protéger le joueur »
├─────────────────────────────────────────────────┤
│ Couche 2 — Sélection d'action (Utility, ~4 Hz)  │  score(heal), score(dps),
│   courbes de réponse sur HP, mana, menace, rôle  │  score(interrupt)…
├─────────────────────────────────────────────────┤
│ Couche 1 — Exécution & réflexes (tick moteur)   │  CreatureAI callbacks,
│   MotionMaster, cast, esquive de void zone       │  déjà fournis par TC
└─────────────────────────────────────────────────┘
        ▲ toutes les couches lisent/écrivent un BLACKBOARD partagé
```

**Pourquoi pas un pur Behavior Tree ?** Un BT est un bon choix par défaut,
mais l'arbitrage combat (le cœur du problème compagnon) s'exprime
naturellement en scores, pas en priorités fixes d'un Selector — c'est
exactement le retour d'expérience de mod-playerbots (relevance) et des
jeux à support/heal (Guild Wars 2). Le BT redevient pertinent comme *format
d'exécution des tâches HTN* si besoin plus tard.

### 3.2 Composants à introduire (nouveau module `src/server/game/AI/Companion/`)

1. **`Blackboard`** — état partagé hiérarchique (par compagnon + par groupe) :
   cible focus, rôle assigné, danger connu, position de regroupement, mémoire
   court-terme. Simple `flat_hash_map` clé→variant ; c'est le prérequis de la
   coordination et il n'existe nulle part dans TrinityCore.
2. **`UtilitySelector`** — actions candidates déclarées avec leurs
   *considérations* (courbes de réponse sur des entrées normalisées 0–1 :
   HP allié le plus bas, mana, nb d'ennemis, menace relative). Scores
   multiplicatifs (modèle IAUS). **Définir les courbes en données**
   (JSON/DB) pour itérer sans recompiler — même philosophie que
   `smart_scripts`, mais avec un modèle expressif.
3. **`GroupDirector`** — un « cerveau de groupe » (un par groupe de
   compagnons, pas par NPC) qui fait tourner le HTN léger : choisit le but
   courant, assigne les rôles, poste les décisions sur le blackboard de
   groupe. C'est le pattern rôle/groupe de Horizon Zero Dawn appliqué à un
   party de 2–5.
4. **`CompanionAI : public CreatureAI`** — point d'insertion unique dans le
   moteur : les callbacks événementiels alimentent le blackboard, `UpdateAI`
   consomme la décision courante et pilote `MotionMaster`/casts. Enregistré
   via le `CreatureAIRegistry` existant → **zéro modification invasive du
   core**.

### 3.3 Optimisations de la boucle (rendues possibles par le contexte solo)

- **Budgets de réflexion étagés** : réflexes au tick moteur, utility à
  200–300 ms, planning à ~1 s ou sur événement (mort d'un membre, wipe de
  plan). Ne pas mettre l'intelligence dans `UpdateAI` à chaque tick.
- **LOD d'IA** (comme le PID d'activité de mod-playerbots) : les NPC hors du
  voisinage du joueur retombent sur l'IA TrinityCore standard ; seuls les
  compagnons + ennemis engagés paient le coût des couches 2–3.
- **Requêtes spatiales mutualisées** : une passe de perception par tick de
  groupe qui remplit le blackboard, au lieu de N appels
  `SelectTarget`/visiteurs de grille par NPC (les visiteurs de grille sont le
  poste de coût n°1 des IA TrinityCore).
- **Rester mono-thread par map** : en solo il n'y a qu'une map active ; ne
  pas chercher à paralléliser l'IA (le modèle mémoire du core ne s'y prête
  pas). Si un jour le planning devient lourd, le déporter en tâche asynchrone
  avec résultat consommé au tick suivant — jamais d'accès concurrent aux
  entités.

### 3.4 Ce qu'il ne faut PAS faire

- **Ne pas étendre SmartScripts** pour les compagnons : le modèle
  événement→action plat ne porte ni arbitrage ni plans ; on aboutirait à des
  centaines de lignes SQL fragiles.
- **Ne pas copier NPCBots tel quel** : ses 30 fichiers de rotation codés en
  dur sont l'anti-modèle de la règle « simplicity first » du projet — la
  logique de classe doit être des *données* (actions + courbes), pas du code.
- **Ne pas réécrire la hiérarchie `UnitAI`** : elle est le contrat avec tout
  le moteur (spells, combat, mouvement). On s'insère dessous, on ne la
  remplace pas.
- **Pas de LLM dans la boucle de combat** : si la dimension lore appelle du
  dialogue génératif/mémoire narrative, l'isoler dans un service hors tick
  (couche narrative), le comportemental restant local et déterministe.

### 3.5 Feuille de route incrémentale suggérée

| Étape | Livrable | Valide |
|---|---|---|
| 1 | `CompanionAI` squelette enregistré dans le registry, follow + assist basiques | l'insertion moteur |
| 2 | `Blackboard` + `UtilitySelector` avec 5–6 actions (attack, heal, interrupt, flee, follow, idle) et courbes en données | l'arbitrage |
| 3 | Rotations de classe exprimées en données (actions + considérations par classe) | la maintenabilité |
| 4 | `GroupDirector` HTN léger : rôles tank/heal/dps, buts « engage / retreat / regroup » | la coordination |
| 5 | LOD d'IA + perception mutualisée | la performance |
| 6 | (Optionnel) couche narrative : mémoire épisodique, dialogue SLM local | l'immersion lore |

Chaque étape est jouable et testable seule (règle 4 du CLAUDE.md :
objectifs vérifiables).

---

## 4. ECS (Entity-Component-System) : opportunité pour ISC ?

L'ECS apporte trois choses distinctes, à évaluer séparément :

1. **Composition plutôt qu'héritage** — une entité est un sac de composants,
   le comportement émerge de leur combinaison.
2. **Layout mémoire orienté données** (SoA, itération cache-friendly) — c'est
   de là que vient le gain de performance.
3. **Parallélisme par système** — chaque système itère sur ses composants
   indépendamment.

### 4.1 Le retrofit complet est à exclure

TrinityCore est bâti sur une hiérarchie OOP profonde (`Object` →
`WorldObject` → `Unit` → `Creature`/`Player`) avec identité par pointeur
partout : le protocole réseau (update fields), les spells, le combat, les
scripts — tout référence des `Unit*`. Convertir cela en ECS revient à
réécrire le core, pas à le refactorer (contraire à la règle « changements
chirurgicaux » du projet).

Surtout, le bénéfice n°2 est **quasi nul pour ISC** : l'itération
cache-friendly paie quand on met à jour des dizaines de milliers d'entités
homogènes par frame. En solo : ~1 map active, quelques dizaines de NPC
pertinents. Les goulots d'étranglement de TrinityCore sont ailleurs
(visiteurs de grille, requêtes spatiales).

### 4.2 Ce qu'il faut prendre de l'ECS : la composition

Le principe n°1 est exactement ce dont le module compagnons a besoin — et
c'est la conclusion à laquelle Treeston arrivait dans la refonte IA avortée
de TrinityCore (PR #22461, voir §5) avec ses *behavior flags* : plutôt que
des sous-classes d'IA qui surchargent des callbacks, des **composants de
comportement** attachés/détachés dynamiquement (« auto-attaque : oui/non »,
« tuable : non », « rôle : heal ») que le core exécute.

Appliqué à l'architecture du §3 : le blackboard devient un conteneur de
composants ; les considérations utility et les tâches HTN sont des données
composables par NPC. De l'ECS « spirituel », sans la machinerie.

### 4.3 Quand une vraie lib ECS se justifierait

Uniquement si un sous-système avec beaucoup d'éléments homogènes mis à jour
en masse apparaît — typiquement une simulation de « monde vivant »
(centaines de NPC ambiants avec routines hors écran). Dans ce cas : un
registre **EnTT** (header-only, facile à vendorer dans `dep/`) *local à ce
sous-système*, synchronisé avec les `Creature` réelles seulement à proximité
du joueur. À ne pas introduire avant d'avoir ce besoin.

---

## 5. Retour d'expérience : les tentatives de refonte chez TrinityCore

Cinq PRs upstream documentent les débats internes sur une révision majeure
de l'IA et de la boucle d'update. Les quatre premières forment la « chaîne
Treeston » (2018-2019, toutes fermées non mergées), la cinquième illustre
l'approche qui fonctionne aujourd'hui.

### 5.1 [#22268 — Map-wide update ticks](https://github.com/TrinityCore/TrinityCore/pull/22268) (fermée)

Supprimer l'itération « joueurs → grilles voisines → marquage → update » au
profit d'un tick global sur tout ce qui est chargé.

- Ovahlord : inquiétude coût RAM/CPU sur les continents.
- Mesure terrain (klister803) : diff de ~60 → 200-300 ms avec 100-200
  joueurs ; Shauren : hausse attendue mais excessive.
- **xvwyh, l'idée la plus utile du thread : l'update « en vagues »** (chaque
  cellule a un ordre d'update, le travail est étalé sur N ticks) ;
  robinsch/Palabola : fréquences différenciées par type de tâche.

*Pertinence ISC : faible* — le problème (coût du marquage avec des milliers
de joueurs) n'existe pas en solo. Mais l'idée des vagues/fréquences
différenciées rejoint le LOD d'IA du §3.3 et resservira si un « monde
vivant » hors de la vue du joueur est simulé un jour.

### 5.2 [#22296 — AI update throttling](https://github.com/TrinityCore/TrinityCore/pull/22296) (fermée)

Passer l'update d'IA de chaque tick à ~1 Hz, comme retail.

- Objection des scripteurs (Traesh) : les timers de boss (ex. Lich King)
  exigent une précision de 0,5 s — throttler casse les scripts existants.
- Krudor : séparer les timers de sorts du « think » de l'IA.
- **Le commentaire pivot (xvwyh)** : l'IA retail est un système *data-driven*
  qui « pense » (choix de sorts par priorité/faisabilité) pendant que le core
  *exécute* les conséquences ; l'IA TrinityCore est un script codé en dur qui
  exige la précision à la milliseconde. **On ne peut pas throttler l'IA TC
  sans d'abord changer son modèle.**

*Pertinence ISC : directe* — c'est le fondement des « budgets de réflexion
étagés » du §3.3, avec la leçon en plus : throttling **opt-in par IA**
(`CompanionAI` et ennemis engagés seulement), jamais global, pour ne pas
casser les scripts legacy.

### 5.3 [#22902 — Targeting revamp](https://github.com/TrinityCore/TrinityCore/pull/22902) (fermée)

Préparatif : séparer victime d'auto-attaque / cible principale / unité
sélectionnée, nettoyage d'API. Techniquement sain, peu contesté — mort avec
le reste de la chaîne.

### 5.4 [#22461 — The AI system redesign](https://github.com/TrinityCore/TrinityCore/pull/22461) (fermée)

La pièce maîtresse : callbacks gérés côté core (seuils de vie, timers),
*behavior flags*, 99 % des scripts en Lua+DB, un « ActionScript » séquentiel
pour les passages mis en scène, suppression de SmartAI.

Positions notables dans la discussion (76 commentaires) :

- **xvwyh** documente le système réel de Blizzard (données extraites du
  client + screenshots WowEdit) : des **creature spell lists** (sorts +
  cooldowns min/max + poids de probabilité relatifs, listes échangeables par
  phase), des *triggersets* événementiels, des *actionsets* avec attentes,
  et des sorts « primer » server-side pour la sélection de cibles. Thèse :
  « les scripts devraient être uniquement des données ».
- **ratkosrb** confirme via l'interview d'Alex Brazie (designer d'encounters
  WoW original) : pas de langage de script en vanilla, tout en données via
  l'éditeur interne.
- **Naios** (méthode) : séparer le problème perf (à *mesurer* avant d'agir)
  du problème d'ergonomie d'API ; futures/coroutines contre le callback hell.
- **jameyboor** (contradiction) : « Blizzard le fait » n'est pas un
  argument ; exiger des bénéfices concrets ; risque de finir avec un système
  à 50 %. **Warpten** : débogabilité des callbacks.
- **BAndysc** (WoWDatabaseEditor) : concevoir le système *avec* son éditeur.
- Épilogue (janvier 2019) : PR fermée pour cause de burnout de son auteur —
  la refonte reposait sur une seule personne.

### 5.5 [#30602 — SummonInfo API](https://github.com/TrinityCore/TrinityCore/pull/30602) (Ovahlord, 2025 — **mergée**)

Le contre-exemple méthodologique : une classe centralisée pour le
comportement des invocations, migrée **fonction par fonction**, « testé en
jeu, aucun changement de comportement observé », review courte et technique.
Six ans après la chaîne Treeston, c'est *cette* approche incrémentale qui
fait avancer les révisions majeures chez TrinityCore.

### 5.6 Synthèse pour ISC

**Sur le fond, la chaîne Treeston valide l'architecture du §3.** « Le core
exécute, l'IA pense à basse fréquence », les behavior flags, les scripts
comme données : même diagnostic que la pile utility + blackboard. Mieux :
les *creature spell lists* retail (sorts pondérés, cooldowns min/max,
listes par phase) sont littéralement **un sélecteur d'utilité pondéré
primitif** — le modèle recommandé est une généralisation de ce que faisait
Blizzard en 2004.

**Sur la méthode, elle montre le piège à éviter.** #22461 est morte de son
périmètre : big-bang, « on jette tout l'ancien système », dépendante d'une
personne. #30602 montre ce qui survit : petits pas mergés, coexistence
ancien/nouveau, zéro régression par étape — d'où la feuille de route
incrémentale du §3.5.

Trois emprunts concrets pour la roadmap :

1. **Throttling opt-in par IA** (étape 1-2) : « think » à ~250 ms-1 s
   uniquement pour `CompanionAI`, les scripts legacy gardent leur tick.
2. **Spell lists en données** (étape 3) : table type `companion_spell_list`
   (spell, cooldown min/max, poids, condition de rôle/phase) — précédent
   retail documenté ; c'est la moitié du sélecteur d'utilité déjà faite.
3. **Behavior flags composables** plutôt que sous-classes d'IA
   supplémentaires — le point de jonction avec l'ECS (§4.2).

Deux garde-fous issus des threads : **mesurer avant d'optimiser** (Naios —
en solo le coût d'`UpdateAI` est probablement négligeable ; l'intérêt du
throttling est d'offrir un budget de pensée propre, pas de sauver des ms)
et **chaque étape doit avoir un bénéfice jouable démontrable** (jameyboor),
pas seulement « une meilleure architecture ».

---

## 6. Sources

**Patterns d'IA de jeu :**
- [Game AI Planning: GOAP, Utility, and Behavior Trees](https://tonogameconsultants.com/game-ai-planning/)
- [GOBT: A Synergistic Approach to Game AI Using Goal-Oriented and Utility-Based Planning in Behavior Trees](https://www.jmis.org/archive/view_article?pid=jmis-10-4-321)
- [Beyond State Machines: Utility AI, Behavior Trees, and GOAP](https://www.socratopia.app/library/game-code-anatomy-en/chapter-12)
- [Nez framework — AI (FSM, Behavior Tree, GOAP, Utility AI)](https://anshuman-kumar.gitbook.io/nez-doc/ai-fsm-behavior-tree-goap-utility-ai)

**Utility AI / IAUS :**
- [Utility Theory Crash Course (Curvature wiki)](https://github.com/apoch/curvature/wiki/Utility-Theory-Crash-Course)
- [Intrinsic Algorithm — Infinite Axis Utility System (Dave Mark)](https://www.gameai.com/iaus.php)
- [Utility system — Wikipedia](https://en.wikipedia.org/wiki/Utility_system)

**HTN / planning en production :**
- [HTN Planning in Decima (Guerrilla Games)](https://www.guerrilla-games.com/read/htn-planning-in-decima)
- [The AI of Horizon Zero Dawn — AI and Games](https://aiandgames.com/hzd-part1/)
- [Behind The AI of Horizon Zero Dawn](https://www.gamedeveloper.com/design/behind-the-ai-of-horizon-zero-dawn-part-1-)

**Projets bots WoW :**
- [trickerer/Trinity-Bots (NPCBots, TrinityCore 3.3.5)](https://github.com/trickerer/Trinity-Bots)
- [liyunfan1223/mod-playerbots (architecture Strategy/Trigger/Action, DeepWiki)](https://deepwiki.com/liyunfan1223/mod-playerbots)
- [mod-playerbots/mod-playerbots (AzerothCore)](https://github.com/mod-playerbots/mod-playerbots)

**Refontes TrinityCore (PRs upstream) :**
- [#22268 — Map-wide update ticks (Treeston, 2018)](https://github.com/TrinityCore/TrinityCore/pull/22268)
- [#22296 — AI update throttling (Treeston, 2018)](https://github.com/TrinityCore/TrinityCore/pull/22296)
- [#22902 — Targeting revamp, AI refactor prep (Treeston, 2019)](https://github.com/TrinityCore/TrinityCore/pull/22902)
- [#22461 — The AI system redesign (Treeston, 2018)](https://github.com/TrinityCore/TrinityCore/pull/22461)
- [#30602 — SummonInfo API (Ovahlord, 2025, mergée)](https://github.com/TrinityCore/TrinityCore/pull/30602)
- [Actions de script Blizzard extraites du client (xvwyh)](https://docs.google.com/spreadsheets/d/1Wh1e1ZXYM-sw3wWrY9dGZ2ngK8oa3PA2XIftQJuf5R0/edit?usp=sharing)
- [Interview Alex Brazie (ex-designer d'encounters WoW) — Countdown to Classic ep. 75](https://countdowntoclassic.com/2018/10/15/episode-75-ex-wow-dev-alex-brazie-on-monster-raid-encounter-design-blizzard-classic/)

**ECS :**
- [EnTT — bibliothèque ECS C++ header-only](https://github.com/skypjack/entt)

**LLM/SLM et NPC :**
- [A General Review of Large Language Model Agents in Game Applications (2025)](https://dl.acm.org/doi/10.1145/3783862.3783876)
- [LLM-Driven NPCs: Cross-Platform Dialogue System](https://arxiv.org/html/2504.13928v1)
- [Fixed-Persona SLMs with Modular Memory: Scalable NPC Dialogue on Consumer Hardware](https://arxiv.org/pdf/2511.10277)
- [A Framework for Complementary Companion Character Behavior in Video Games](https://arxiv.org/pdf/1808.09079)
