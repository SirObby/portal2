"""Integrity failures must reject data, rather than silently shift its labels."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import render_alignment as alignment


class AlignmentIntegrityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.video = root / "sample.mp4"
        self.video.write_bytes(b"video bytes; decoder is mocked in unit tests")
        self.demo = root / "sample.dem"
        header = bytearray(1072)
        header[:8] = b"HL2DEMO\0"
        struct.pack_into("<i", header, 8, 4)
        struct.pack_into("<fii", header, 1056, 1.0, 60, 3)
        packets = b"".join(struct.pack("<BiB", 2, tick, 0) + bytes(160)
                           + struct.pack("<i", 0) for tick in (2, 4, 6))
        self.demo.write_bytes(header + packets + b"\x07")
        self.sidecar = root / "sample.mp4.alignment.jsonl"
        self.header = dict(type="header", schema="portal2-render-alignment-v1",
                           capture_stage="after_host_render", blend=1, fps=60,
                           time_base_num=1, time_base_den=60, width=256, height=192,
                           demo=str(self.demo), demo_sha256=alignment.digest(self.demo))
        self.frames = [dict(type="frame", frame_index=i, pts=i, demo_tick=2 + i,
                            client_time=i / 60, paused=False, skipping=False,
                            camera_interpolation=[dict(target_tick=2 + i,
                                previous_packet_tick=2, next_packet_tick=6,
                                observed_client_time=i / 60), None]) for i in range(3)]
        self.footer = dict(type="end", complete=True, frame_count=3,
                           video_sha256=alignment.digest(self.video))
        self.decoded = dict(streams=[dict(width=256, height=192, r_frame_rate="60/1")],
                            frames=[dict(best_effort_timestamp_time=i / 60) for i in range(3)])

    def write(self):
        self.sidecar.write_text("".join(json.dumps(r) + "\n"
                               for r in [self.header, *self.frames, self.footer]))

    def validate(self):
        self.write()
        with patch.object(alignment, "probe_video", return_value=self.decoded):
            return alignment.validate(self.video, self.sidecar)

    def test_complete_mapping_and_packet_brackets(self):
        _, _, report = self.validate()
        self.assertTrue(report["validated"])
        self.assertEqual(report["slot0_camera_observations"], 3)

    def test_truncated_or_incomplete_render_rejected(self):
        self.footer["complete"] = False
        with self.assertRaisesRegex(ValueError, "complete cleanly"):
            self.validate()

    def test_repeated_playback_ticks_preserved(self):
        self.frames[1]["demo_tick"] = 2
        _, _, report = self.validate()
        self.assertEqual(report["playback_tick_delta_counts"][0], 1)

    def test_frame_index_gap_rejected(self):
        self.frames[1]["frame_index"] = 2
        with self.assertRaisesRegex(ValueError, "Non-contiguous"):
            self.validate()

    def test_video_substitution_rejected(self):
        self.video.write_bytes(b"other video")
        with self.assertRaisesRegex(ValueError, "Video SHA-256"):
            self.validate()

    def test_demo_substitution_rejected(self):
        self.demo.write_bytes(self.demo.read_bytes() + b"changed")
        with self.assertRaisesRegex(ValueError, "Demo SHA-256"):
            self.validate()

    def test_decoded_timestamp_shift_rejected(self):
        self.decoded["frames"][1]["best_effort_timestamp_time"] = .2
        with self.assertRaisesRegex(ValueError, "timestamp mismatch"):
            self.validate()

    def test_missing_decoded_frame_rejected(self):
        self.decoded["frames"].pop()
        with self.assertRaisesRegex(ValueError, "count mismatch"):
            self.validate()

    def test_invented_camera_bracket_rejected(self):
        self.frames[0]["camera_interpolation"][0]["previous_packet_tick"] = 1
        with self.assertRaisesRegex(ValueError, "not found in source packets"):
            self.validate()

    def test_stale_camera_observation_rejected(self):
        self.frames[0]["camera_interpolation"][0]["observed_client_time"] = 12
        with self.assertRaisesRegex(ValueError, "Stale"):
            self.validate()

    def test_missing_camera_observation_remains_unlabeled(self):
        self.frames[1]["camera_interpolation"][0] = None
        _, _, report = self.validate()
        self.assertEqual(report["frames_without_slot0_camera_timing"], [1])


if __name__ == "__main__":
    unittest.main()
