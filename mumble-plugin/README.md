# OpenGOAL Jak 1 Positions — Mumble plugin

Relays every player's in-game position through the Mumble server so the mod
can draw other players in the world (blue spheres, see `draw-mumble-peers` in
`goal_src/jak1/engine/mods/mod-custom-code.gc`).

## How it works

```
your game  --shared memory-->  your Mumble + this plugin
    --plugin data channel (via the Mumble server)-->
other players' plugins  --shared memory-->  their games
```

- Positions are raw game units end-to-end; the `*mumble-world-scale*` audio
  tuning does not affect them.
- Broadcasts at 5 Hz, only while you're actually in-game (title screen and
  loads stop the broadcast; stale peers disappear after ~3 seconds).
- Requires **Mumble 1.5+** and every participating player must install the
  plugin. Voice chat itself works without it — this only adds positions.

## Install (every player)

1. Build or grab `opengoal_jak1_positions.dll` (see below).
2. In Mumble: **Configure → Settings → Plugins → Install plugin...** and pick
   the DLL. Make sure the plugin is enabled in the list afterwards.
3. Run the game (this mod). The plugin logs
   `OpenGOAL Jak 1: connected to game shared memory.` in Mumble's console
   once it finds the game.

## Build

Windows (clang-cl, from this directory):

```
clang-cl /LD /O2 /std:c++17 /EHsc /Iinclude opengoal_jak1_positions.cpp /Fe:opengoal_jak1_positions.dll
```

or with CMake: `cmake -B build -S . && cmake --build build --config Release`

> Note: on Windows use clang-cl or MSVC, **not** plain `clang++`/`g++` —
> `MumblePlugin.h` selects a no-op export attribute under `__GNUC__` and the
> plugin's symbols won't be exported.

`include/MumblePlugin.h` is vendored unmodified from the
[mumble-voip/mumble](https://github.com/mumble-voip/mumble) 1.5.x branch
(BSD-licensed, see its header).
