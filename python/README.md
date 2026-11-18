# portal2-gym

Gymnasium environment for Portal 2 (`samuelhautamaki/Portal2-v0`), backed
by a custom Source engine plugin. See `../docs/protocol.md` for the
plugin <-> environment wire protocol, and `../cpp/` for the plugin source.

### Automatic Source demos

Enable per-episode native demo recording with:

```python
from portal2_gym import Portal2Env

env = Portal2Env(record_demo=True, demo_dir="artifacts/demos")
obs, info = env.reset()  # starts a uniquely named Source .dem
# ... interact with the environment ...
metadata = env.finish_demo()  # call at an external time limit / early stop
print(metadata["demo"], metadata["demo_sha256"])
env.close()
```

`demo_port` optionally selects the owned game's loopback netconsole port
(default: control port + 1). An existing `-netconport` in `launcher_kwargs`
is reused; conflicting ports are rejected. No SAR is required. With
`_skip_launch=True`, the external launcher must enable that netconsole port.
The default `record_demo=False` preserves existing Gym callers; the
portal-speedrun project requires it for every live trial.

Recording starts after reset, and finalizes automatically on native map/CM
termination, the next reset, or close. External wrappers such as `TimeLimit`
must call `env.unwrapped.finish_demo()` when they truncate if they need the
metadata immediately. It is idempotent and returns `last_demo`. A finalized
episode cannot be stepped again until reset when recording is enabled.

Source's `stop` command is queued while the engine waits for actions. Finalizing
therefore sends **five neutral post-episode steps**; these do not appear in the
policy trajectory or overwrite the returned terminal observation. Completion
metrics must use the pre-flush terminal state. Demo verification checks the
Source header, expected map, positive duration/ticks, then copies it to
`demo_dir` and computes SHA256. Validation errors propagate; a missing demo is
not silently treated as a complete recording. The game installation retains its
original demo copy. Abrupt process termination cannot guarantee finalization.
Native recordings are of the game simulation; RGB video remains a separate
optional artifact. Recording can affect engine timing; evaluate with recording
enabled rather than extrapolating performance from unrecorded trials.
