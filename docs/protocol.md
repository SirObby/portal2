# Portal2-RL IPC Protocol

This document is the single source of truth for communication between the
Python Gymnasium environment (`portal2_gym`) and the C++ Source engine
plugin (`portal2_rl_plugin`). Both sides must stay in sync with this spec.

## Overview

Two channels are used:

1. **Control socket** (TCP, localhost) — JSON-lines messages for handshake,
   actions, state/reward-relevant observations, episode control (reset,
   restart map), and synchronization (tick lockstep).
2. **Shared memory frame buffer** — raw RGB24 pixel data for the rendered
   game frame, written by the plugin once per step and read by Python.
   A TCP socket is used instead of a named pipe / domain socket for
   cross-platform simplicity (Linux + Windows both support `AF_INET`
   loopback sockets identically).

Shared memory is implemented via POSIX `shm_open`/`mmap` on Linux and
`CreateFileMapping`/`MapViewOfFile` on Windows, wrapped behind a common
`SharedFrameBuffer` interface (see `cpp/include/shared_frame_buffer.h`).
The shared memory **name** is communicated to Python during the handshake
over the control socket so both sides agree on it.

## Lockstep model

The plugin hooks the engine's per-tick `GameFrame` callback. On each
engine tick:

1. The plugin renders the frame, writes pixels into the shared memory
   buffer, serializes the current game state, and sends a `TICK`
   message over the control socket.
2. The plugin **blocks** (engine frame does not advance further) until it
   receives an `ACTION` message from Python.
3. The plugin applies the action (sets button/movement/view-angle state
   for the next `usercmd`), unblocks, and the engine proceeds to the next
   tick.

This guarantees exact tick-by-tick correspondence between RL steps and
game ticks (default 15ms / 66.67 Hz, i.e. `interval_per_tick = 1/66.667`
unless the map's `sv_alternateticks` / tickrate differs — Portal 2 SP
default is 0.015).

## Connection sequence

1. Python launches the Portal 2 process with the plugin pre-configured to
   load (`+plugin_load portal2_rl_plugin` and an autoexec config that sets
   the control port).
2. The plugin's `Load()` opens a TCP listen socket on
   `127.0.0.1:<PORT>` (default `27182`, configurable via launch option
   `+portal2_rl_port <PORT>`).
3. Python connects as the TCP client and sends `HELLO`.
4. Plugin responds with `HELLO_ACK`, including the shared memory segment
   name and the negotiated frame resolution.
5. Python opens the shared memory segment.
6. Normal tick loop begins (plugin sends `TICK`, Python replies `ACTION`).

## Message framing

All control messages are newline-delimited UTF-8 JSON objects
(`json.dumps(...) + "\n"`), sent over the TCP socket. Each message has a
`"type"` field.

### `HELLO` (Python → Plugin)

```json
{"type": "HELLO", "width": 1280, "height": 960}
```

Requests a frame resolution. The plugin will attempt to match the game's
actual `-w`/`-h` launch resolution; `width`/`height` here are informational
and used to size the shared memory buffer. They **must** match the
resolution the game was launched with (Python controls both, so this is
guaranteed by construction).

### `HELLO_ACK` (Plugin → Python)

```json
{
  "type": "HELLO_ACK",
  "shm_name": "/portal2_rl_frame_27182",
  "width": 1280,
  "height": 960,
  "channels": 3,
  "tick_interval": 0.015
}
```

`shm_name` is the platform-appropriate shared memory identifier, suffixed
with the plugin's control port (`portal2_rl_frame_<port>`) so multiple
Portal 2 instances can run concurrently on the same machine -- each
instance is launched with a distinct `+portal2_rl_port`, giving each a
distinct shm segment name with no collision:
- Linux: name passed to `shm_open` (leading `/`, e.g. `/portal2_rl_frame_27182`)
- Windows: name passed to `CreateFileMapping` (e.g. `Local\portal2_rl_frame_27182`)

The frame buffer size in bytes is `width * height * channels`.

The native handshake also reports `input_backend` (`console`, `server`, or
`client`), `challenge_requested` (whether opt-in Challenge Mode is requested),
and `sar_submission_blocked` (whether the loaded SAR cheated-run guard was
confirmed). A false SAR guard is expected when SAR is absent. The default input
backend is `console`; `PORTAL2_RL_DIRECT_INPUT=1` selects server usercmd delivery,
and `PORTAL2_RL_DIRECT_INPUT=client` selects client CreateMove delivery.

### `TICK` (Plugin → Python)

Sent once per engine tick, after the frame has been written to shared
memory. Python must respond with `ACTION` before the plugin will allow
the engine to proceed.

```json
{
  "type": "TICK",
  "tick": 14523,
  "frame_ready": true,
  "state": {
    "pos": [x, y, z],
    "vel": [x, y, z],
    "ang": [pitch, yaw, roll],
    "on_ground": true,
    "portal1": {"placed": true, "pos": [x, y, z], "normal": [x, y, z]},
    "portal2": {"placed": false, "pos": [0,0,0], "normal": [0,0,0]},
    "map": "sp_a2_triple_laser",
    "map_finished": false,
    "in_loading": false
  }
}
```

Field notes:

- Top-level diagnostic fields, also present in `RESET_ACK`: `engine_tick`
  (native tick counter), `input_consumed` (last consumed action request ID,
  initially -1), `input_pending` (queued/in-flight actions), `simulating`,
  and `paused`. Direct input is queued; a returned observation does not prove
  that its corresponding action has already been consumed.
- Top-level Challenge Mode fields: `challenge_enabled` (native cvar value),
  `challenge_nodes` (discovered finish-node count), `challenge_finished`
  (local player's native finish-node touch), `challenge_hud_hidden`, and
  `challenge_seconds` (native challenge timer). They are distinct from the
  request tick counter and `state.map_finished`. Opt in with
  `PORTAL2_RL_CHALLENGE_MODE=1` in the game process environment.

- `pos`, `vel`: player origin / velocity, Source units (inches), `[x, y, z]`.
- `ang`: player eye angles (pitch, yaw, roll) in degrees.
- `on_ground`: from `FL_ONGROUND` flag.
- `portal1`/`portal2`: portal placement state, world position, and surface
  normal of each portal (blue = portal1, orange = portal2). `placed=false`
  means the portal has not been fired / does not currently exist.
- `map`: current map name (string).
- `map_finished`: true from the tick a level-transition fires (see "Level
  transitions" below) onward, until the next `RESET`. Python uses this as
  the episode-success/termination signal; reward is computed in Python.
  Portal 2 has no health/death mechanic worth tracking in single-player,
  so the only built-in termination signal is `map_finished`; anything
  else (getting stuck, time limits) is handled by Python-side wrappers
  (e.g. `gymnasium.wrappers.TimeLimit`) using `pos`/`vel` heuristics.
- `in_loading`: true while a level transition / loading screen is active.
  No frame is meaningfully renderable; `frame_ready` will be `false` and
  Python should not read the shared memory buffer on this tick (treat the
  observation as a repeat of the last valid frame, or zeros on the very
  first tick).

### Level transitions

Portal 2 SP maps end with a `point_changelevel` entity that the player
walks into (an elevator, typically), which fires the engine's internal
`changelevel`/`changelevel2` command and starts loading the next map
autonomously -- the engine does this regardless of what Python wants.

The plugin treats this as a terminal condition rather than letting the
game actually progress to the next chamber:

1. The plugin hooks/polls for `point_changelevel`'s touch/use firing (or
   detects the engine's pending level-change command before it executes).
2. On detection, the plugin immediately freezes input handling (as if
   `RESET` were received, see `ResetInputState()`) and sets
   `map_finished = true` on the *current* tick's `TICK` message -- sent
   before the engine actually transitions, so Python still gets a frame
   from the original chamber.
3. The plugin does not block the engine from proceeding to load the next
   map (this is a normal user-driven level transition the engine
   considers routine), so for a few ticks the engine may load whatever
   map sequentially follows the current one in the campaign while it
   waits for Python's next message.
4. Python, on observing `terminated=True` (derived from `map_finished`),
   calls `reset()`, which sends `RESET` unconditionally with whichever
   map name the caller wants (typically the *same* map, to repeat the
   episode) -- this forces a `map <name>` command that overrides whatever
   the engine was autonomously loading, so the agent never meaningfully
   plays in chamber N+1 unless Python explicitly asks for it via
   `options={"map": ...}` on `reset()`.

This means there is a short window (a handful of ticks, bounded by how
quickly Python reacts to `terminated=True`) where the engine is loading
an unrequested map before `RESET` reclaims control. Since `in_loading`
is true throughout this window, Python should not treat any of those
frames/states as meaningful observations.

### `ACTION` (Python → Plugin)

```json
{
  "type": "ACTION",
  "move_forward": 1.0,
  "move_side": -0.5,
  "pitch_delta": 0.0,
  "yaw_delta": 2.5,
  "jump": false,
  "duck": false,
  "use": false,
  "fire_portal1": false,
  "fire_portal2": true
}
```

Field semantics (applied to the `usercmd` for the next tick):

| Field           | Type  | Range      | Meaning                                            |
|-----------------|-------|------------|-----------------------------------------------------|
| `move_forward`  | float | [-1, 1]    | Forward(+)/backward(-) movement axis               |
| `move_side`     | float | [-1, 1]    | Strafe right(+)/left(-) axis                       |
| `pitch_delta`   | float | degrees    | Change in view pitch this tick (clamped to ±89°)   |
| `yaw_delta`     | float | degrees    | Change in view yaw this tick                       |
| `jump`          | bool  | -          | `IN_JUMP` button                                   |
| `duck`          | bool  | -          | `IN_DUCK` button                                   |
| `use`           | bool  | -          | `IN_USE` button                                    |
| `fire_portal1`  | bool  | -          | `IN_ATTACK` (places blue portal)                   |
| `fire_portal2`  | bool  | -          | `IN_ATTACK2` (places orange portal)                |

`move_forward`/`move_side` are scaled to `cl_forwardspeed`/`cl_sidespeed`
internally by the plugin.

**`jump` and `use` are pulse buttons; `fire_portal1`/`fire_portal2`/`duck`
are held buttons.** The plugin issues real `+`/`-` console commands
(`IVEngineServer::ClientCommand`) to apply actions, mirroring exactly how
a human pressing/holding a bound key behaves:

- `fire_portal1`, `fire_portal2`, `duck` are **held**: sending `true`
  issues `+attack`/`+attack2`/`+duck` once and it stays active every tick
  you keep sending `true`; sending `false` issues the matching `-` command.
- `jump`, `use` are **pulses**: sending `true` issues `+jump`/`+use` for
  exactly that one tick, and the plugin automatically sends the matching
  `-jump`/`-use` at the very start of the next tick, regardless of what
  that next action requests. This means every `jump=true` in an `ACTION`
  message reliably produces exactly one jump (Portal 2 has no
  auto-bunnyhop -- holding the raw button does not repeat-jump), and you
  do not need to manually alternate `jump=true`/`jump=false` across
  multiple steps to get a clean press.

### `RESET` (Python → Plugin)

Sent by Python instead of `ACTION` when `env.reset()` is called. Tells the
plugin to issue `map <mapname>` and wait for the level to finish loading
before resuming the tick loop.

```json
{"type": "RESET", "map": "sp_a2_triple_laser"}
```

### `RESET_ACK` (Plugin → Python)

Sent once the new map has finished loading and the first valid tick/frame
is available. Has the same `state` shape as `TICK` (with `tick` reset to
the new session's tick counter).

```json
{"type": "RESET_ACK", "tick": 0, "frame_ready": true, "state": { ... }}
```

### `CLOSE` (Python → Plugin)

```json
{"type": "CLOSE"}
```

Plugin unblocks the engine permanently (stops waiting for actions) and
closes the socket. Python is then expected to terminate the game process.

## Episode boundaries & termination

- `terminated`: derived in Python from `state.map_finished` (the agent
  reached the level's `point_changelevel` / exit elevator -- success).
  Portal 2 SP has no health/death mechanic, so this is the only
  default plugin-reported termination signal. When the handshake confirms
  `challenge_requested`, native `challenge_finished` also terminates the episode.
  Python `info` distinguishes `map_transition_finished` and `challenge_finished`.
  Stuck/soft-lock detection (e.g. no
  position change for N steps) is left to Python-side wrappers.
- `truncated`: time-limit handled by Python (`max_episode_steps`), standard
  Gymnasium `TimeLimit`-style behavior -- the plugin has no concept of this.
- On `terminated`/`truncated`, Python sends `RESET` to restart the map,
  which also reclaims control from the engine's autonomous level load
  (see "Level transitions" above).

## Versioning

This is protocol version `1`. Both `HELLO`/`HELLO_ACK` may later gain a
`"version"` field for compatibility checks; for v0.1 it is omitted and
assumed to match.

### Optional Python demo recording

`Portal2Env(record_demo=True, ...)` uses the owned game's separate loopback
netconsole (`-netconport`, default control port + 1) for Source `record`/`stop`.
This does not add protocol messages or require a C++ rebuild. Five neutral
`ACTION`/`TICK` exchanges after the episode execute and finalize queued `stop`;
these are outside the policy trajectory. See `python/README.md` for lifecycle
and artifact validation details.
