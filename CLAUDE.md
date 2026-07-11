# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ISC (Immersive SoloCraft) is a fork of **TrinityCore** (a WoW 3.3.5 MMORPG server framework), being adapted from a
massively-multiplayer server into a client/server framework for a standalone, lore-focused single-player RPG. The
client/server architecture is preserved, but the server (worldserver/authserver) is being extended and optimized to
support intelligent NPC bots/companions and other single-player-specific features. This is an educational project;
no commercial use or mass distribution is intended.

**Coding rules:**
1. **Think before coding.** State assumptions out loud. If a request is ambiguous, ask instead of guessing. If a
   simpler approach exists, push back on the request rather than silently implementing the more complex one.
2. **Simplicity first.** Write the minimum code that solves the problem. No speculative abstractions, no
   flexibility nobody asked for.
3. **Surgical changes.** Touch only what the task requires. Don't refactor or "improve" neighboring code. Every
   changed line should trace back to the request.
4. **Goal-driven execution.** Turn vague instructions into verifiable targets before writing code (e.g. "add
   validation" → "write tests for invalid inputs, then make them pass").

**Toolchain baseline:** C++20, CMake ≥ 4, Boost ≥ 1.90, OpenSSL ≥ 3, MySQL ≥ 9.

## Build

Out-of-source build required (in-source builds are blocked by CMake policy).

```sh
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTOOLS=1 -DSCRIPTS=static -DSERVERS=1
make -j$(nproc)
make install   # installs authserver/worldserver + configs to CMAKE_INSTALL_PREFIX
```

**Do not configure or build unless explicitly asked**. Builds are slow (CMake + compile of a large C++ codebase) and rarely needed to make code changes.

## Repository layout

**`src/common/`** — networking (Asio), crypto, config, logging, shared utilities.
**`src/server/game/`** — core gameplay; compiled into worldserver.
**`src/server/scripts/`** — content scripts grouped by region (EasternKingdoms/, Northrend/, …), class (Spells/spell_mage.cpp, …), and domain (Commands/, Pet/, OutdoorPvP/, World/).
**`src/server/database/`** — DB abstraction and schema updater.
**`src/server/shared/`** — code shared by auth and world servers.
**`src/server/{authserver,worldserver}/`** — entry points (ports 3724 and 8085).
**`src/tests/`** — Test unit tests + mocks.
**`sql/`** — migration files to update the database. Use only update folder to create migrations.
**`dep/`** — vendored third-party dependencies.
