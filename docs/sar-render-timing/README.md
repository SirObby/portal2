# SAR rendering and frame alignment patches

These patches target SourceAutoRecord revision
`99f1d314783e47f0c9dd13fb25ed71d3ff0c2b79` and were tested on Linux Portal 2
build 8928 at 256×192, 60 fps, with blending disabled.

Apply in this order from a clean SAR checkout:

```sh
git apply /path/to/portal2-rl/docs/sar-render-timing/capture-on-render.patch
git apply /path/to/portal2-rl/docs/sar-render-timing/frame-alignment.patch
make -j4 VERSION=1.15.2-4-g99f1d314-local-alignment-v1 sar.so
```

Stop Portal 2 before installing the built `sar.so` in its `portal2` directory.
Install the accompanying `portal2_rl_render.cfg` in `portal2/cfg`. No hot-unload
or hot-reload is required. Building SAR requires its normal dependencies and a
32-bit toolchain. These are local patches, not an upstream release.

## What changes

`capture-on-render.patch` moves capture from the outer engine loop to immediately
after `_Host_RunFrame_Render`. An outer-loop iteration does not guarantee a new
image; capturing there can repeatedly encode the same buffer and distort speed.

`frame-alignment.patch` adds optional `sar_render_alignment 1` sidecars beside
videos. Pixels and their metadata share the encoder buffer lock. Sidecars are
written as `.partial` and finalized only after successful encoder flush, mux
trailer, video hash and file close. A complete footer means clean capture
shutdown, not proof of complete demo coverage.

The config enables alignment, 60 fps, blend 1, no merging, H.264 quality 50
(CRF 0), and 256×192-compatible visual settings. SAR still converts to YUV420;
CRF 0 is not pixel-exact RGB capture.

## Tick semantics and validation

- `demo_tick`: playback cursor at pixel readback.
- `camera_interpolation[slot].target_tick`: the observed camera interpolation
  request, with its previous/next packet ticks and client clock.

These are different clocks. Camera metadata is null if stale or outside a valid
packet bracket. Do not invent a constant offset or assume frame zero is tick
zero. SAR's playback fix can omit initial ticks, and entities may have their own
interpolation. Co-op, merged video and blended capture are not validated here.

`tools/render_alignment.py` checks demo/video hashes, decoded frame counts, PTS,
dimensions, playback order and camera packet brackets. The dataset normalizer
also fixes MP4 final-packet/edit-list visibility without changing decoded images.

See [dataset preparation](../datasets.md) for the complete pipeline and scientific
admission limits. Historical console logs, comparison renders and pilot outputs
are local artifacts and are intentionally excluded from this source repository.


## Gun-free observations

The config hides the portal gun with `r_drawviewmodel 0` and
`viewmodel_offset_x 5000`, immediately and in both demo-start and config-reload
callbacks. The reload callback previously restored offset zero, so its final
one-time hiding command was insufficient. The corrected config was verified in
a fresh 50-demo Triple Laser render, with early/middle/late sample frames checked
across all 49 unique demonstrations. Held world objects remain visible.
The live Gym launcher already requests offset 5000; local evaluation also sets
`r_drawviewmodel 0`. Keep dataset rendering and policy observations consistent.

## Challenge Mode HUD and research submission policy

The render config uses `sar_disable_challenge_stats_hud -1` and
`sar_portalgun_hud 0`, including demo-start/config-reload callbacks. The former
hides the built-in CM stats; the latter disables SAR's separate portalgun HUD.
`cl_drawhud 0` alone does not hide the built-in bottom-right CM panel.

Research configs keep `sv_cheats 1` across those callbacks. At the pinned SAR
revision, `AutoSubmit::FinishRun` returns immediately for a cheated run; development
builds also return unconditionally. There is no autosubmit-off cvar at this
revision. The native bridge additionally sets SAR's exported
`AutoSubmit::g_cheated` guard when SAR is loaded. Live evaluation requires
confirmation of that guard before accepting actions with SAR enabled. No API-key
files are renamed, read by the evaluator, or removed.

The default live path remains SAR-free. Its opt-in Challenge Mode path directly
hides `CHUDChallengeStats` and blocks the leaderboard-open callback, matching the
rendered appearance without loading SAR. A leaderboard popup can pause simulation;
verify that neutral unfocused calibration keeps advancing, and do not count
paused client frames as simulated steps. The SAR-enabled direct-input path remains unvalidated; use the validated
SAR-free path for policy runs. The renderer pins the verified development SAR
binary and confirms `sv_cheats=1` before playback. Its one-demo pilot completed
and confirmed the native cheated-run guard during capture.
