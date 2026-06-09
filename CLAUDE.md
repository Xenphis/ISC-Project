# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**ISC (Immersive SoloCraft)** is a fork of TrinityCore (WoW 3.3.5a server emulator) adapted for a single-player, local standalone experience. The focus is on AI enhancement and player immersion. It is an educational project — no commercial use.

## Build System

The project uses **CMake** with an out-of-source build in `build/`. A `build/` directory already exists with a `Debug` configuration.

### Configure & Build

```bash
# From project root — must build out-of-source
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DSCRIPTS=static -DTOOLS=1 -DBUILD_TESTING=1
make -j$(nproc)
make install
```

Key CMake flags:
| Flag | Default | Purpose |
|---|---|---|
| `SCRIPTS` | `static` | `none/static/dynamic/minimal-static/minimal-dynamic` |
| `TOOLS` | `1` | Build map/vmap/mmap extraction tools |
| `BUILD_TESTING` | `0` | Build Catch2 test suite |
| `WITH_COREDEBUG` | `0` | Extra debug assertions in core |
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | `Debug/Release/RelWithDebInfo/MinSizeRel` |
| `CMAKE_INSTALL_PREFIX` | — | Install destination for binaries and configs |

### Run Servers (VS Code tasks or manual)

```bash
# authserver
./build/bin/Debug/bin/authserver -c ./build/bin/Debug/etc/authserver.conf

# worldserver
./build/bin/Debug/bin/worldserver -c ./build/bin/Debug/etc/worldserver.conf
```

VS Code tasks `Launch authserver` and `Launch worldserver` are pre-configured in [.vscode/tasks.json](.vscode/tasks.json).

### Run Tests

```bash
cd build
ctest --output-on-failure
# or run the test binary directly
./build/bin/Debug/bin/unit_tests
```

### Code Style Check

```bash
bash contrib/check_codestyle.sh
```

Rules enforced: 4-space indentation (no tabs), no trailing whitespace, no consecutive blank lines, `ObjectGuid::ToString()` instead of `GetCounter()` in logs. C/C++ files use `latin1` encoding (not UTF-8).

## Architecture

### Server Processes

Two binaries cooperate:
- **authserver** — handles login/authentication, account management. Entry: [src/server/authserver/Main.cpp](src/server/authserver/Main.cpp).
- **worldserver** — the game world simulation. Entry: [src/server/worldserver/Main.cpp](src/server/worldserver/Main.cpp).

### Source Tree

```
src/
├── common/          # Shared utilities: logging, crypto, threading, networking, configuration
├── server/
│   ├── authserver/  # Auth process
│   ├── worldserver/ # World process
│   ├── database/    # DB abstraction layer (async query system)
│   ├── shared/      # Networking, packets, realm info, secrets
│   └── game/        # Core game logic (see below)
├── tools/           # Map/vmap/mmap extractors and assemblers
└── genrev/          # Git revision stamp generator
dep/                 # Vendored dependencies (boost, openssl, mysql, recastnavigation, …)
sql/
├── base/            # Full DB dumps for auth, characters, world
├── updates/         # Incremental SQL updates (auth/, characters/, world/)
└── custom/          # ISC-specific SQL additions
tests/               # Catch2 unit tests (game/ and common/ subdirs)
```

### Game Layer (`src/server/game/`)

| Directory | Role |
|---|---|
| `AI/` | All creature/unit AI — see below |
| `Entities/` | Core game objects: `Player`, `Creature`, `Unit`, `GameObject`, `Pet`, `Totem`, `Vehicle`, … |
| `Handlers/` | Opcode handlers — one file per system (chat, movement, combat, …) |
| `Maps/` | Map loading, grid management, instance handling |
| `Movement/` | `MotionMaster`, movement generators, `PathGenerator` (uses RecastNavigation) |
| `Scripting/` | `ScriptMgr` — the hook system that binds C++ scripts to game events |
| `Spells/` | Spell system, aura mechanics |
| `Scripts/` (in `src/server/scripts/`) | Concrete script implementations |

### AI Subsystem

The AI layer is the primary extension point for ISC's immersion goals:

- **`UnitAI`** ([src/server/game/AI/CoreAI/UnitAI.h](src/server/game/AI/CoreAI/UnitAI.h)) — base class for all AI.
- **`CreatureAI`** ([src/server/game/AI/CreatureAI.h](src/server/game/AI/CreatureAI.h)) — overrides for creature-specific events (combat, movement, gossip, …).
- **`ScriptedAI`** ([src/server/game/AI/ScriptedAI/ScriptedCreature.h](src/server/game/AI/ScriptedAI/ScriptedCreature.h)) — convenience base for script-authored creature AI.
- **`SmartAI`** ([src/server/game/AI/SmartScripts/SmartAI.h](src/server/game/AI/SmartScripts/SmartAI.h)) — data-driven AI driven by the `smart_scripts` DB table; useful for quick NPC behaviours without recompilation.
- **`PlayerAI`** — AI that can take over player control (e.g., charm).

Specialised cores: `PetAI`, `GuardAI`, `TotemAI`, `ReactorAI`, `PassiveAI`, `CombatAI`, `ScheduledChangeAI`.

### Script System

Scripts are statically linked C++ translation units compiled into the worldserver. The entry points are the `AddXxxScripts()` functions collected in [src/server/scripts/ScriptLoader.cpp.in.cmake](src/server/scripts/ScriptLoader.cpp.in.cmake).

**Adding a custom script:**
1. Create `src/server/scripts/Custom/MyScript.cpp`.
2. Implement your `CreatureAI` / `ScriptMgr` hook subclass.
3. Add a `void AddMyScriptScripts()` function and call it from `AddCustomScripts()` in [src/server/scripts/Custom/custom_script_loader.cpp](src/server/scripts/Custom/custom_script_loader.cpp).
4. Register the source file in the `Custom/` `CMakeLists.txt`.
5. Recompile.

`ScriptMgr` ([src/server/game/Scripting/ScriptMgr.h](src/server/game/Scripting/ScriptMgr.h)) exposes hooks for: creature AI, player events, spells, instance scripts, map events, weather, world events, and more.

### Database Layer

Three logical databases:
- **auth** — accounts, realms, bans.
- **characters** — per-character persistent state.
- **world** — static game data (creatures, quests, items, smart_scripts, …).

Queries go through an async system in `src/server/database/`. Schema changes use the `sql/updates/` directory with a prefix-numbered naming convention. ISC-specific schema lives in `sql/custom/`.

## ISC-Specific Focus Areas

When working on AI or immersion features, the key extension points are:
- **`smart_scripts` table** — data-driven behaviour without recompilation.
- **`ScriptMgr` hooks** — attach to any game event (player login, zone change, NPC interaction, …).
- **`PathGenerator` / `MotionMaster`** — for custom NPC movement and patrol logic.
- **`GossipDef`** ([src/server/game/Entities/Creature/GossipDef.h](src/server/game/Entities/Creature/GossipDef.h)) — NPC dialogue trees.
