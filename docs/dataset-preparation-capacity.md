# Dataset preparation measurements and capacity estimate

Measured locally on 17 November 2026, using the current 256×192, 60 fps,
blend-1, H.264 CRF-0 SAR configuration and the single-game folder pipeline.

## Actual selected dataset size

A **header-only inventory** read 1,072 bytes per demo, plus filesystem sizes.
No other map folder was rendered, and no action/entity payloads outside Triple
Laser were processed for this inventory.

- **3,000 demos**, in **60 map folders**.
- Reported duration: **105,115.872 seconds = 29 h 11 min 55.872 s**.
- Header ticks: **6,306,952**; dividing by 60 gives 105,115.867 seconds.
  The small difference is normal floating-point header rounding.
- Source demo files: **2.373 GB** total (decimal).
- No nonpositive/nonfinite durations, nonpositive ticks, map-folder mismatches,
  or header rates outside 59.5–60.5 Hz were found. This does not prove the
  recordings are complete or every file is renderable. Six demos are shorter
  than one second and may need special boundary treatment.

The filtered source and game demo collections agree on all 3,000 filenames,
map names, header hashes and file sizes. Their sizes are not added together in
the dataset projection; keeping duplicate source copies costs another 2.373 GB.

## Measured full-folder benchmark

The single-game `sar_startdemosfolder` run processed all 50 Triple Laser demos:

| Quantity | Measurement |
| --- | ---: |
| Header content duration | 477.133 s (7 min 57.133 s) |
| Retained video duration | 448.217 s (7 min 28.217 s) |
| Launch, folder playback, state capture, render, shutdown | 173.849 s |
| Pixel verification, normalization, alignment checks, export | 68.520 s |
| Entire preparation run | **243.379 s (4 min 3.379 s)** |
| Validated rendered frames | 26,893 |
| Exact camera frame references | 26,178 |
| Original user commands | 33,999 |
| Retained complete per-command client states | 33,895 |

All 50 episodes passed processing and validation. Native state has the Gym's
18-field layout. Every normalized RGB frame was compared with its source hash;
video counts/PTS/dimensions, file hashes and camera brackets were also checked.
The complete benchmark manifest is saved separately so a subsequent resume-only
run cannot overwrite its timing record.

## Full-dataset time estimate

The first-order duration-scaled projection is:

```text
243.379 seconds preparation / 477.133 seconds demo content
× 105,115.872 seconds actual dataset content
= 53,618 seconds preparation ≈ 14.9 hours
```

**Plan for 15–24 hours on this machine.** This is a measured extrapolation from
one map, not a guarantee. Different maps, loading time, motion/encoding cost,
longer demos, retries, and system load change throughput. Per-folder launches
are amortized over many demos. The benchmark has unusually short demos, so
simple duration scaling can overcount fixed per-file overhead; more complex
maps can offset that advantage.

This estimate covers the current automated file preparation and validation
pipeline. It does not estimate the research/development time needed to validate
causal original-simulation/frame alignment or change the Gym actuator. Failed
or unrenderable files require separate handling and are not assumed successful.

## Full-dataset storage estimate

GB below are decimal, matching file byte totals. The source inputs use their
actual inventoried size. JSON/audit data use measured bytes per header second.
Video and frame mappings use measured bytes per **retained video second**, with
the observed 0.578 s average startup trim applied once per demo (3,000 times),
rather than repeatedly scaling short-demo startup loss by duration.

| Retained component | Estimated GB |
| --- | ---: |
| Normalized video | 102.3 |
| Frame mapping sidecars | 2.2 |
| Observation/action JSONL | 20.2 |
| Per-demo native state/command journals | 5.9 |
| Original demos | 2.4 |
| Status metadata | 0.03 |
| **Retained total** | **133.0 GB (~123.8 GiB)** |

**Budget 130–160 GB for the retained dataset.** The JSONL is uncompressed, and
video settings remain those measured above. Map-dependent visual complexity
can substantially change the lossless-quantization video size.

Keeping raw SAR videos as well adds about 103.0 GB; raw mappings, immutable
folder journals and logs add about another 8.1 GB. Thus the measured projection
for **all current-run outputs/intermediates is about 244 GB (~227.3 GiB)**.
Reserve **about 300 GB of working space** (rough planning range 270–320 GB),
plus any historical renders or duplicate pilot runs retained separately.

The script currently preserves raw SAR output; it does not automatically prune
it after validation. Reducing disk usage by dropping redundant intermediates or
compressing JSON would be a separate storage-policy change.

## Reproduction and artifacts

Use `tools/inventory_demo_durations.py <selected-root> --output inventory.json`
for a metadata-only duration/count inventory, and `tools/prepare_demo_folder.py`
for actual preparation. See [the workflow](datasets.md).

Measured inventories, benchmark manifests and raw reports are local artifacts,
kept outside the source repository. The scripts reproduce these measurements;
generated data are ignored by Git.
`training_ready` remains false until the separate causal alignment and Gym
actuator-equivalence questions are resolved; completion of these processing
checks is not an assertion that those research checks have passed.
