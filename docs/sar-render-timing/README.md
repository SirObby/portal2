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
