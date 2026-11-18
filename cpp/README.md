# portal2_rl_plugin

A Source engine server plugin that bridges Portal 2 to the
`portal2_gym` Gymnasium environment. See `../docs/protocol.md` for the
wire protocol and `../README.md` for the overall project layout.

## Prerequisites

- CMake >= 3.16
- A C++17 compiler
- **32-bit build support**, since Portal 2 is a 32-bit engine on both
  Linux and Windows:
  - Linux: `sudo apt install gcc-multilib g++-multilib` (Debian/Ubuntu)
    or your distro's equivalent. Confirm with `g++ -m32 -dumpmachine`.
  - Windows: use a Visual Studio "x86" toolset / generator, or MinGW's
    32-bit (`i686-w64-mingw32`) toolchain.
- Internet access at configure time (CMake's `FetchContent` downloads
  `nlohmann/json`'s single header from `raw.githubusercontent.com`).

## Building (Linux)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

Produces `build/portal2_rl_plugin.so`. Copy it into your Portal 2
install's `portal2/` directory:

```bash
cp portal2_rl_plugin.so "$HOME/.steam/steam/steamapps/common/Portal 2/portal2/"
```

## Building (Windows)

```powershell
mkdir build; cd build
cmake .. -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Produces `build/Release/portal2_rl_plugin.dll`. Copy it into your Portal 2
install's `portal2\` directory.

## Loading the plugin

`python/portal2_gym/launcher.py` launches Portal 2 with
`+plugin_load portal2_rl_plugin` and `+portal2_rl_port <port>` on the
command line, so normally you don't need to do anything manually. If you
want to test the plugin without going through Python, launch the game
yourself and type in the in-game console:

```
plugin_load portal2_rl_plugin
portal2_rl_port 27182
```

(note: `portal2_rl_port` as a manual console command has no effect after
`Load()` has already run and parsed the command line -- it's only read
once at startup. For manual testing, set the port via the launch command
line argument `+portal2_rl_port <port>` before `+plugin_load`, or just
use the default `27182`.)

No `-insecure` launch flag is needed -- unsigned plugins load fine in
Portal 2 singleplayer via `plugin_load`.

## How interface resolution works (and what to do if it breaks)

Portal 2 ships no public Source SDK headers, so the plugin resolves
engine interfaces and entity fields at runtime rather than linking
against typed structs from a vendor SDK. This is the same approach
[SourceAutoRecord](https://github.com/p2sr/SourceAutoRecord) (SAR) uses,
and `vendor/sar/` vendors the relevant (MIT-licensed) parts of it:

- `vendor/sar/Interface.{hpp,cpp}` -- resolves an interface by calling
  the target module's exported `CreateInterface(versionString, ...)` and
  optionally wraps it so individual vtable slots can be hot-swapped.
- `vendor/sar/Utils/Memory.{hpp,cpp}` -- raw process-memory helpers
  (`Deref`, `DerefDeref`, `Read`, `VMT` for reading a vtable slot,
  `Scan` for byte-pattern signature scanning) and the `Hook` class (a
  5-byte inline JMP patch for hooking a function by address rather than
  by vtable slot -- used for `PlayerRunCommand`, since we want to patch
  the function's actual code once rather than just one instance's vtable
  pointer).
- `vendor/sar/Entity.{hpp,cpp}` -- reads/writes entity fields by *name*
  (e.g. `SE(ent)->field<bool>("m_bActivated")`) by walking the engine's
  own datamap/SendTable reflection at runtime. This is far more robust
  to game updates than hardcoded byte offsets, since it only breaks if
  Valve renames a field (rare) rather than whenever any unrelated code
  shifts in the binary.
- `vendor/sar/Offsets.{hpp,cpp}` + `vendor/sar/Offsets/Portal 2 9568.hpp`
  -- the small set of engine-side offsets that genuinely can't be
  resolved by name (vtable slot indices for `GameFrame`,
  `PlayerRunCommand`, `ReadScreenPixels`, etc., plus a couple of
  byte-offset chains for globals like `gpGlobals` and `videomode`). This
  table is copied directly from SAR's repository, tagged for exe build
  9568 -- confirmed by SAR's own source comment to also match build 8928
  ("Default Public Version" as of mid-2026, `PatchVersion=2.0.0.1`),
  which is what this plugin is built and tested against.

**If Valve patches Portal 2 and the plugin stops working** (it will log
warnings to stdout/stderr and `Load()` will return `false`, refusing to
activate, rather than silently misbehaving), the most likely fix is:
check SourceAutoRecord's repository for a newer `Offsets/Portal 2
<build>.hpp` file and swap it in at `vendor/sar/Offsets/Default.hpp`'s
`#include`. Since this plugin only depends on a handful of named offsets
(see the list above), a newer table will almost always be a drop-in
replacement.

## What's vendored vs. original

Everything under `vendor/sar/` is copied (in some cases lightly trimmed
of unrelated features) from SourceAutoRecord and is MIT licensed; see
`vendor/sar/LICENSE`. Everything under `src/` is original code written
for this project:

- `src/Engine.hpp/.cpp` -- resolves the handful of interfaces/globals we
  need (`IServerGameDLL`, `gpGlobals`, `UTIL_PlayerByIndex`,
  `m_EntPtrArray`, `videomode`, `IVEngineServer::ClientCommand`) and
  exposes `ReadScreenPixels()` / `QueueConsoleCommand()`.
- `src/RLState.hpp/.cpp` -- extracts player + portal state into the
  `GameState` struct sent over the wire.
- `src/RLInput.hpp/.cpp` -- hooks `CPortal_Player::PlayerRunCommand` to
  apply the action Python sends for the current tick.
- `src/RLBridge.hpp/.cpp` -- the protocol state machine: handshake,
  per-tick `TICK`/`ACTION` exchange, `RESET`/`RESET_ACK`, and level-
  transition detection.
- `src/Plugin.cpp` -- the `IServerPluginCallbacks` implementation Portal 2
  actually loads; wires the engine's lifecycle callbacks to `RLBridge`.
- `src/ControlSocket.hpp`, `src/SharedFrameBuffer.hpp` -- the two IPC
  primitives described in `../docs/protocol.md`.

## Validating a build without Portal 2

Since this sandbox/dev environment can't load the plugin into a running
Portal 2 process, every file here has been individually compiled and the
whole plugin has been linked into a real `.so` (validated against a
64-bit toolchain in absence of 32-bit dev libraries in that environment;
your real build should use the `-m32` flags already configured in
`CMakeLists.txt`). The vtable-slot/offset values themselves can only be
fully validated by actually running the plugin in-game -- if you hit a
crash on `Load()`, the most likely culprits are an offset that's drifted
for your exact build, or a calling-convention mismatch on
`PlayerRunCommand` (try logging inside `PlayerRunCommand_Detour` first
to confirm it's even being called before debugging further).

## Read-only demo dataset capture

The plugin now also supports a separate client-demo capture mode, activated by
`PORTAL2_RL_DEMO_CAPTURE=/absolute/output/capture.jsonl.partial` in the game's
environment. It bypasses the live RL socket and action/lockstep path. Load it
before SAR; it observes replayed client entities and recorded command decoding.
Use the [folder preparation command](../docs/datasets.md) to coordinate
this mode, provenance, rendering, clean journal shutdown and export validation.
Normal Gym launches without this variable retain the live bridge behavior.

## Unfocused live RL without SAR

Live RL mode now installs the same `CInputSystem::SleepUntilInput` hook used by
SAR's `sar_disable_no_focus_sleep`: it forwards to the original input function
with a maximum sleep of zero. Input processing still runs. The existing vendored
`Offsets::SleepUntilInput` slot (33 on both supported platforms) is used; no
`engine_no_focus_sleep` cvar is assumed.

This is enabled by default when SAR is not explicitly requested. Disable it for
an A/B benchmark by passing `+portal2_rl_no_focus_sleep 0` in launcher extra args.
It is skipped in demo-capture mode, and when `+portal2_rl_has_sar 1` requests SAR's
own fix. Missing interfaces/hooks warn and leave the environment usable. Unload
restores the original InputSystem vtable. As with other vtable hooks, manually
loading another plugin that wraps the same interface requires reverse load-order
unloading; avoid hot-reloading plugins during a training run.

The source is adapted from SAR's MIT-licensed InputSystem module. The 32-bit Linux build was tested live with SAR disabled on Triple Laser.
Two unfocused repeats per setting produced 112 distinct frames with the hook off
and 172 with it on over 360 steps. A settled-camera turn appeared after 3–5 steps
without the hook and after one step with it. Environment stepping stayed near
60 steps/s in both cases: the measured benefit was observation freshness, not a
large step-throughput increase. Focus was checked during every trial. These are
scene-specific measurements, not a general performance guarantee. Windows
runtime and engine hot-unload/reload remain untested.

## Experimental command delivery and Challenge Mode

`PORTAL2_RL_DIRECT_INPUT=1` queues actions for server `PlayerRunCommand`;
`PORTAL2_RL_DIRECT_INPUT=client` queues them in client `CreateMove` before
prediction. Defaults retain the legacy console backend. Opt-in
`PORTAL2_RL_INPUT_TRACE=/absolute/path.jsonl` records requested, created and
consumed commands. Direct modes preserve held button bits and half-speed analog
movement. A single action is queued once; observations can precede its consumption.
The handshake reports `input_backend`; ticks report `input_consumed` and
`input_pending`. These are experimental Linux/build-8928 checks, not a claim of
complete actuator equivalence or Windows validation.

`PORTAL2_RL_CHALLENGE_MODE=1` enables CM after `ApplyGameSettings` resets its
cvar. Initial startup reloads once if necessary to create the native finish node.
Ticks expose `challenge_enabled`, `challenge_nodes`, `challenge_finished` and
`challenge_seconds`. A finish uses SAR's native StartTouch predicate: the local player touches a real,
previously uncompleted node. A map transition remains a separate signal.
The base Gym terminates on the native finish when CM was requested; ordinary
episodes still use `map_finished`. Challenge HUD/leaderboard suppression matches the rendering
config and prevents leaderboard popups from pausing a background experiment.

When SAR is explicitly loaded, the bridge sets SAR's exported cheated-run guard
and waits for confirmation before its handshake. This blocks `FinishRun` before
it can submit a score. The render cfg also enforces `sv_cheats 1`. SAR stays
off by default. Hot-unloading SAR during an experiment is unsupported.
