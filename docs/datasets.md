# Demo dataset preparation

The pipeline targets Linux Portal 2 single-player with the installed SAR
alignment build, the Steam Scout runtime, and the supplied render config.

## Prepare a map folder

```sh
cmake --build cpp/build -j4
python tools/prepare_demo_folder.py /path/to/demos/sp_a2_triple_laser \
  --output /path/to/processed/sp_a2_triple_laser --resume
```

The default launches one Portal 2 process and runs `sar_startdemosfolder` once.
Native samples include the demo filename, so the folder journal can be split
into per-demo observations afterward. A partial resume queues only unfinished
files through symlinks. `--limit 2` selects a pilot subset.

Only `.dem` files directly inside the supplied folder are selected. The command
refuses to commandeer an existing listener on the netconsole port (default
27666). Override paths with `--game`, `--runtime`, `--plugin`, `--config`, and
`--port`; run `--help` for defaults. The command closes the game it launches.

Load Portal2-RL **before SAR**: the capture mode resolves the engine's original
client-state vtable. The pipeline handles this order automatically. For building
the required SAR renderer, see [sar-render-timing](sar-render-timing/README.md).

## Processing and output

The command captures replayed client entities before each recorded usercmd is
decoded, then checks the engine-decoded command against an independent parse of
the original demo. It handles SAR's startup replay pass explicitly and excludes
paused/skipping samples. Capture mode never injects actions or waits for Python.

It renders a same-run video and records camera timing, copies H.264 packets into
a normalized video without re-encoding, and verifies every decoded RGB frame hash
against its source. Normalization supplies final packet durations and disables
MP4 edit lists so ordinary decoders expose every frame. Training output omits
audio. Validation checks file hashes, decoded frame counts, PTS, dimensions and
camera packet brackets.

```text
<output>/<demo>/
  episode.jsonl              observations, actions and frame references
  capture.jsonl              native state/command journal
  video.mp4                  normalized video
  video.mp4.alignment.jsonl  hash-bound frame mapping
  status.json                provenance, validation and failures
<output>/folder-captures/    immutable journals named by SHA-256
<output>/capture-latest.json
<output>/capture-plan.json
<output>/manifest.json
<output>/game.log
<output>/console.log
```

`--resume` checks input/tool/config identities, export and journal hashes, and
video alignment before reusing an episode. Failed work retains logs and error
status. Raw SAR renders are preserved beside the input demos; they are not
pruned automatically. Keep generated output outside this source repository.

## Data semantics

Each episode has a schema/provenance header, tick records, and a completion
footer. Record ticks, command ticks, playback cursor ticks and camera target
ticks remain separate. Negative setup ticks and gaps are preserved. Multiple
commands at one record tick remain separate, with their own native observations;
a top-level state is populated only for an unambiguous slot-0 sample.

The state matches the Gym's 18-field layout: player origin, velocity, view angles,
grounded state, and active flags/positions for both portals. Unavailable fields
are null with false validity masks. Packet camera origins are retained separately
and never substituted for player origin.

Actions preserve original sequence numbers, command tick, angles, movement,
held-button mask, impulse, weapon selection, signed mouse deltas and compressed
payload. The nine-field Gym candidate uses an explicit movement scale (default
175), shortest signed yaw differences and contiguous command predecessors.
Missing deltas remain null; out-of-bounds values are reported, never clipped.
Portal 2 extension bits remain preserved in the original payload.

Frame references are zero-based presentation indices, joined only by exact
observed camera target tick. Repeated targets keep multiple frames. Missing
camera observations are not filled with a guessed image.

`complete: true` means processing and its integrity checks passed.
`training_ready` remains false until causal original-simulation/frame alignment
and live Gym actuator equivalence have been validated. In particular:

- A state before usercmd **decoding** is not proof of the original server's state
  before that action was **simulated**. Client entities and images interpolate.
- The live Gym uses direction-only movement keys despite a continuous action Box,
  pulses jump/use, and limits turns more narrowly than some demonstrations.
- Startup/boundary coverage and episode success need explicit definitions. A demo
  ending is not proof of successful level completion.

Group train/test splits by runner and near-duplicate run, rather than randomly
splitting adjacent frames from the same episode. No synthetic reward is needed
or fabricated for behavioral cloning.

## Standalone tools and checks

```sh
python tools/inventory_demo_durations.py /path/to/selected/demos --output inventory.json
python tools/export_demonstrations.py /path/to/demos --output /path/to/export
python tools/normalize_demo_video.py /path/to/demos --output /path/to/export/videos
python tools/render_alignment.py /path/to/video.mp4 --tick 138
python -m unittest discover -s tools -p 'test_*.py'
```

The inventory reads only 1,072-byte demo headers and filesystem sizes. The
standalone exporter supports `--video-directory`, `--video-suffix`, and
`--capture-directory` for joining separately prepared artifacts.

The parser follows [the demo format documentation](https://dem.nekz.me/) and
Valve's `src/game/shared/usercmd.cpp`. See the [capacity measurements](dataset-preparation-capacity.md)
for the 50-demo benchmark and estimates based on the full dataset's actual duration.
