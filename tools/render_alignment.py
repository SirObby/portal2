#!/usr/bin/env python3
"""Validate a SAR alignment sidecar and look up frames by demo camera tick.

Uses ffprobe to decode/count frames and compare presentation timestamps. Frame
indices are zero-based presentation order, never compressed packet/DTS order.
Only an exact recorded camera target tick matches --tick; no nearest-tick guess.
"""
import argparse
from collections import Counter
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def read_mapping(path):
    require(not str(path).endswith(".partial"), "Partial sidecar cannot be accepted")
    with path.open() as source:
        records = [json.loads(line) for line in source]
    require(len(records) >= 3, "Sidecar has no frames/footer")
    header, *frames, footer = records
    require(header.get("schema") == "portal2-render-alignment-v1", "Unknown schema")
    require(header.get("type") == "header", "Missing header")
    require(header.get("capture_stage") == "after_host_render", "Wrong capture stage")
    require(header.get("blend") == 1, "Blended images cannot have one tick label")
    require(footer.get("type") == "end" and footer.get("complete") is True,
            "Render did not complete cleanly")
    require(footer.get("frame_count") == len(frames), "Footer/frame count mismatch")
    previous = None
    for index, row in enumerate(frames):
        require(row.get("type") == "frame", f"Invalid frame row {index}")
        require(row.get("frame_index") == index and row.get("pts") == index,
                f"Non-contiguous frame index/PTS at {index}")
        tick = row.get("demo_tick")
        require(type(tick) is int and tick >= 0, f"Invalid demo tick at {index}")
        require(previous is None or tick >= previous, f"Playback seek/regression at {index}")
        require(not row.get("skipping"), f"Demo skipping at frame {index}")
        require(not row.get("paused"), f"Paused demo at frame {index}")
        require(math.isfinite(row["client_time"]), f"Invalid client clock at {index}")
        previous = tick
        require(len(row["camera_interpolation"]) == 2, "Expected two slot observations")
        for camera in row["camera_interpolation"]:
            if camera is not None:
                require(abs(camera["observed_client_time"] - row["client_time"]) <= .00001,
                        f"Stale camera observation at {index}")
    return header, frames, footer


def packet_ticks(path):
    """Read protocol-4 command framing, not compressed network/usercmd payloads."""
    ticks = set()
    size = path.stat().st_size
    with path.open("rb") as source:
        header = source.read(1072)
        require(header[:8] == b"HL2DEMO\0" and struct.unpack_from("<i", header, 8)[0] == 4,
                "Packet verification requires a protocol-4 Source demo")
        while True:
            command = source.read(1)
            require(bool(command), "Demo ended without stop command")
            command = command[0]
            if command == 7:
                return ticks
            tick, _slot = struct.unpack("<iB", source.read(5))
            if command in (1, 2):
                if command == 2 and tick >= 0:
                    ticks.add(tick)
                source.seek(2 * 76 + 8, 1)
            elif command == 3:
                continue
            elif command in (5, 8):
                source.seek(4, 1)  # usercmd number or custom callback index
            else:
                require(command in (4, 6, 9), f"Unknown demo command {command}")
            length, = struct.unpack("<i", source.read(4))
            require(0 <= length <= size - source.tell(), "Invalid demo payload size")
            source.seek(length, 1)


def probe_video(path):
    result = subprocess.run([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames",
        "-show_streams", "-show_entries",
        "stream=width,height,r_frame_rate:frame=best_effort_timestamp_time",
        "-of", "json", str(path)
    ], text=True, capture_output=True, check=True)
    require(not result.stderr.strip(), "Decoder reported errors: " + result.stderr.strip())
    return json.loads(result.stdout)


def validate(video, sidecar, demo=None):
    header, frames, footer = read_mapping(sidecar)
    video_hash = digest(video)
    require(video_hash == footer.get("video_sha256"), "Video SHA-256 does not match sidecar")
    decoded = probe_video(video)
    require(len(decoded["streams"]) == 1, "Expected one selected video stream")
    stream = decoded["streams"][0]
    require((stream["width"], stream["height"]) == (header["width"], header["height"]),
            "Video dimensions disagree with sidecar")
    time_base = Fraction(header["time_base_num"], header["time_base_den"])
    require(time_base > 0 and Fraction(stream["r_frame_rate"]) == header["fps"],
            "Video rate disagrees with sidecar")
    require(time_base == Fraction(1, header["fps"]), "Unexpected time base")
    require(len(decoded["frames"]) == len(frames), "Decoded frame/sidecar count mismatch")
    for row, image in zip(frames, decoded["frames"]):
        timestamp = float(image["best_effort_timestamp_time"])
        require(abs(timestamp - float(row["pts"] * time_base)) < .000002,
                f"Presentation timestamp mismatch at frame {row['frame_index']}")

    demo = Path(demo or header["demo"])
    demo_hash = digest(demo)
    require(demo_hash == header.get("demo_sha256"), "Demo SHA-256 does not match sidecar")
    with demo.open("rb") as source:
        demo_header = source.read(1072)
    require(demo_header[:8] == b"HL2DEMO\0", "Invalid companion demo")
    _, total_ticks, _ = struct.unpack_from("<fii", demo_header, 1056)
    require(max(row["demo_tick"] for row in frames) <= total_ticks,
            "Playback tick exceeds demo header range")
    packets = packet_ticks(demo)
    for row in frames:
        for camera in row["camera_interpolation"]:
            if camera is not None:
                previous = camera["previous_packet_tick"]
                following = camera["next_packet_tick"]
                require(previous in packets and following in packets,
                        f"Camera bracket not found in source packets at frame {row['frame_index']}")
                require(previous <= camera["target_tick"] <= following,
                        f"Camera target outside packet bracket at frame {row['frame_index']}")
    delta = Counter(b["demo_tick"] - a["demo_tick"] for a, b in zip(frames, frames[1:]))
    camera_ticks = [r["camera_interpolation"][0]["target_tick"] for r in frames
                    if r["camera_interpolation"][0] is not None]
    report = {
        "video": str(video), "sidecar": str(sidecar), "demo": str(demo),
        "video_sha256": video_hash, "sidecar_sha256": digest(sidecar),
        "demo_sha256": demo_hash, "frame_count": len(frames),
        "first_playback_tick": frames[0]["demo_tick"],
        "last_playback_tick": frames[-1]["demo_tick"],
        "playback_tick_delta_counts": dict(sorted(delta.items())),
        "slot0_camera_observations": len(camera_ticks),
        "slot0_camera_coverage": len(camera_ticks) / len(frames),
        "first_camera_target_tick": camera_ticks[0] if camera_ticks else None,
        "last_camera_target_tick": camera_ticks[-1] if camera_ticks else None,
        "frames_without_slot0_camera_timing": [r["frame_index"] for r in frames
                                                if r["camera_interpolation"][0] is None],
        "validated": True,
        "scope": "frame/PTS/playback and observed camera timing; not decoded usercmd actions",
    }
    return header, frames, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", type=Path)
    parser.add_argument("--sidecar", type=Path)
    parser.add_argument("--demo", type=Path, help="Override demo path after relocating dataset")
    parser.add_argument("--frame", type=int, help="Look up a zero-based presentation frame")
    parser.add_argument("--tick", type=int, help="Find exact camera target tick matches")
    parser.add_argument("--tick-kind", choices=("camera", "playback"), default="camera",
                        help="Tick meaning for --tick; default: camera interpolation target")
    parser.add_argument("--slot", type=int, choices=(0, 1), default=0)
    parser.add_argument("--report", type=Path, help="Save validation summary with file hashes")
    args = parser.parse_args()
    sidecar = args.sidecar or Path(str(args.video) + ".alignment.jsonl")
    try:
        header, frames, report = validate(args.video, sidecar, args.demo)
        if args.frame is not None:
            require(0 <= args.frame < len(frames), "Frame index outside captured range")
            report["frame"] = frames[args.frame]
        if args.tick is not None:
            report["tick_query"] = args.tick
            report["tick_kind"] = args.tick_kind
            if args.tick_kind == "playback":
                report["matching_frames"] = [r for r in frames if r["demo_tick"] == args.tick]
            else:
                report["matching_frames"] = [
                    r for r in frames if r["camera_interpolation"][args.slot] is not None
                    and r["camera_interpolation"][args.slot]["target_tick"] == args.tick
                ]
        if args.report:
            args.report.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError, ZeroDivisionError,
            subprocess.CalledProcessError, struct.error) as exc:
        print(json.dumps({"validated": False, "error": str(exc)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
