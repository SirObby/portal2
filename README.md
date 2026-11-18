# Portal2-RL

A Gymnasium environment for Portal 2, registered as `samuelhautamaki/Portal2-v0`.

## AI Notice.

As a developer, I am a huge hater of "vibecoding" personally. I hold the personal belief that, (after the creation of JavaScript), agentic coding tools (such as Claude Code etc) have been the worst thing to happen to software development.

However, I must admit that this project was written heavily with the help of vibecoding. In order to get this environment done in a reasonable amount of time, as a developer who does not at all write Python regularly at all, and someone who does not have the time to learn how SAR works with the game, it only made sense for me to enlist the help of Claude to write this environment.

(I did not use Claude Code btw, just the (free tier btw) claude.ai web interface, worked fine for this.)

This is just a Gymnasium environment, **I would never contribute AI written code to any project that is not my own**.

## Unironically the most important acknowledgement.

I absolutely admire and acknowledge the developers of the Portal 2 plugin [SourceAutoRecord](https://github.com/p2sr/SourceAutoRecord) and the Portal 2 Speedrunning Community. This Gymnasium environment would not have been possible without the existance of SAR.

## Environment overview:

The project has two halves that talk to each other over a small IPC
protocol (see `docs/protocol.md`):

- **`python/`** -- the Gymnasium environment (`portal2_gym`), built and run
  with `uv`. Implements `reset()`/`step()`/`render()`/`close()`, launches
  the game, and exchanges actions/observations with the plugin.
- **`cpp/`** -- a Source engine plugin (`portal2_rl_plugin.so` /
  `.dll`) that runs inside Portal 2 itself: advances the game tick by
  tick, reads player/portal state and the rendered frame, and applies the
  actions Python sends.

## How it fits together

```
 Python (Gymnasium)                    Portal 2 process
 ┌───────────────────┐    TCP/JSON     ┌────────────────────────┐
 │ Portal2Env         │ <─────────────>│ portal2_rl_plugin.so    │
 │  .reset() / .step()│   control       │  (IServerPluginCallbacks)│
 └──────────┬─────────┘                 └────────────┬───────────┘
            │ shared memory (RGB24 frame)              │
            └──────────────────────────────────────────┘
```

Each engine tick, the plugin captures the rendered frame into shared
memory, reads player/portal state, and sends a `TICK` message over the
control socket -- then blocks until Python replies with an `ACTION`
(or a `RESET`, which issues `map <name>` to restart the level). This is a
true lockstep: the engine does not advance to the next tick until Python
responds, so RL steps correspond exactly to game ticks.

## Why the C++ side looks the way it does

Portal 2 ships no public Source SDK headers (unlike HL2/CS:S-era Source
games), so there's no `IVEngineServer`/`iplayerinfo.h` to link against
directly. The plugin instead vendors the relevant parts of
[SourceAutoRecord](https://github.com/p2sr/SourceAutoRecord) (MIT
licensed; see `cpp/vendor/sar/LICENSE`), which resolves engine interfaces
by symbol lookup and vtable inspection at runtime, and reads entity
fields by name via the engine's own datamap/SendTable reflection. See
`cpp/vendor/sar/` for what's vendored and why, and the comments at the
top of each file in `cpp/src/` for how each piece is used.

The plugin is built and tested against Portal 2 exe build **8928**
("Default Public Version" branch, `PatchVersion=2.0.0.1`), using the
offset table SourceAutoRecord ships for build 9568 -- their own source
comments confirm 8928 matches that table's signatures. If Valve patches
the game and offsets stop resolving (the plugin will log warnings and
refuse to start if any required interface can't be found), check
[SourceAutoRecord's releases](https://github.com/p2sr/SourceAutoRecord)
for an updated offsets file and swap it into `cpp/vendor/sar/Offsets/`.

## Quick start

### 1. Build the plugin

See `cpp/README.md` for full prerequisites and build steps. Short version:

```bash
cd cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

Copy the resulting `portal2_rl_plugin.so` into your Portal 2 install's
`portal2/` directory (next to other game DLLs), or anywhere on the
engine's plugin search path.

### 2. Install the Python package

```bash
cd python
uv venv
uv pip install -e .
```

### 3. Use the environment

```python
import gymnasium as gym
import portal2_gym  # registers samuelhautamaki/Portal2-v0

env = gym.make(
    "samuelhautamaki/Portal2-v0",
    launcher_kwargs={
        # Adjust if your Steam library isn't in the default location.
        # "steam_root": "/home/you/.local/share/Steam",
        # "portal2_dir": "/home/you/.steam/steam/steamapps/common/Portal 2",
    },
)

obs, info = env.reset()
for _ in range(1000):
    action = env.action_space.sample()
    obs, reward, terminated, truncated, info = env.step(action)
    if terminated or truncated:
        obs, info = env.reset()

env.close()
```

`Portal2Env` launches the game itself on first `reset()` (using the same
Steam runtime launch command you'd run by hand), waits for the plugin to
connect, then issues `map sp_a2_triple_laser` (or whatever map you pass
via `options={"map": ...}` to `reset()`) to start each episode.

## Notes

- **Reward is entirely up to you.** The base environment returns
  `reward=0.0` every step and exposes the raw plugin state via
  `info["raw_state"]` (position, velocity, view angles, portal
  placement/position) so you can write reward-shaping wrappers without
  touching the plugin. `terminated=True` fires when the agent reaches the
  level's exit (`point_changelevel`); Portal 2 SP has no health/death
  mechanic, so that's the only built-in signal -- stuck/soft-lock
  detection, time limits, etc. are left to `gymnasium.wrappers`.
- **`-insecure` is not required.** Despite older Valve documentation
  suggesting otherwise, SourceAutoRecord's own install instructions (and
  years of community use) confirm unsigned plugins load fine in Portal 2
  SP via `plugin_load`/`+plugin_load` without it.
- **Unfocused live RL needs no SAR.** The native plugin forwards InputSystem's
  sleep call with zero wait by default. See `cpp/README.md` for the opt-out and
  validation status.
- **Linux-first, Windows-compatible.** All IPC (TCP sockets, shared
  memory) uses cross-platform primitives; see `cpp/src/ControlSocket.hpp`
  and `cpp/src/SharedFrameBuffer.hpp`. The Windows launch path in
  `python/portal2_gym/launcher.py` needs a `portal2_exe` path since
  there's no equivalent of the Linux Steam runtime wrapper script.

## Demonstration datasets

See [dataset preparation](docs/datasets.md) for the single-game map-folder
pipeline, native state capture, resume checks and observation/action semantics.
[Capacity measurements](docs/dataset-preparation-capacity.md) summarize the
full-duration inventory and pilot benchmark. Keep generated data outside this repo.

Python Gym now supports `Portal2Env(record_demo=True, demo_dir="artifacts/demos")`
for automatic native Source recordings, without SAR. See
[the Python recording lifecycle documentation](python/README.md#automatic-source-demos)
for finalization, time-limit handling, netconsole configuration and metadata.
