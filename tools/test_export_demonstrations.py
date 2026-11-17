import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
from export_demonstrations import Bits, decode_usercmd, read_demo, export, action_candidate


class Writer:
    def __init__(self): self.bits = []
    def put(self, value, n): self.bits.extend((value >> i) & 1 for i in range(n))
    def optional(self, value, n):
        self.put(1, 1); self.put(value, n)
    def floating(self, value): self.optional(struct.unpack('<I', struct.pack('<f', value))[0], 32)
    def data(self):
        return bytes(sum(b << j for j, b in enumerate(self.bits[i:i+8])) for i in range(0,len(self.bits),8))


class ExportTests(unittest.TestCase):
    def test_omitted_fields_use_null_baseline(self):
        cmd = decode_usercmd(b'\0\0')
        self.assertEqual((cmd['command_number'],cmd['tick_count']), (1,1))
        self.assertEqual(cmd['buttons'], 0)
        self.assertEqual(cmd['view_angles'], [0,0,0])

    def test_signed_mouse_and_weapon_subtype_zero_selection(self):
        w = Writer()
        w.optional(33,32); w.optional(44,32)
        for v in [4,179,0,175,-175,0]: w.floating(v)
        w.optional(1 | 2 | 2048,32); w.put(0,1)
        w.optional(0,11); w.optional(5,6)
        w.optional(65535,16); w.optional(32768,16)
        cmd = decode_usercmd(w.data())
        self.assertEqual(cmd['weapon_subtype'], 5)
        self.assertEqual((cmd['mouse_dx'],cmd['mouse_dy']),(-1,-32768))
        self.assertEqual(cmd['side_move'], -175)

    def test_no_clipping_and_wraparound(self):
        before = dict(command_number=8,view_angles=[0,179,0])
        cmd = dict(command_number=9,view_angles=[20,-179,0],forward_move=350,side_move=-175,buttons=2049)
        action = action_candidate(cmd,before,175)
        self.assertEqual(action['vector'],[2,-1,20,2,0,0,0,1,1])
        self.assertEqual(action['outside_bounds'],['move_forward','pitch_delta'])
        cmd['command_number'] = 11
        self.assertEqual(action_candidate(cmd,before,175)['vector'][2:4],[None,None])

    def test_truncated_command_rejected(self):
        with self.assertRaises(ValueError): decode_usercmd(b'\1')

    def test_export_preserves_missing_ticks_and_multiple_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'a.dem'
            header=bytearray(1072)
            header[:8]=b'HL2DEMO\0'
            struct.pack_into('<ii',header,8,4,24)
            struct.pack_into('<fiii',header,1056,.05,3,2,0)
            command=lambda tick: struct.pack('<BiBii',5,tick,0,1,2)+b'\0\0'
            path.write_bytes(header+command(1)+command(1)+b'\7')
            target=Path(directory)/'a.jsonl'
            export(path,target)
            rows=[json.loads(x) for x in target.read_text().splitlines()]
            ticks=[r for r in rows if r['type']=='tick']
            self.assertEqual([r['tick'] for r in ticks],[0,1,2,3])
            self.assertEqual(len(ticks[1]['actions']),2)
            self.assertEqual(ticks[0]['actions'],[])
            self.assertEqual(ticks[1]['observations']['state'],[None]*18)
            self.assertFalse(ticks[1]['quality']['training_ready'])
            path.write_bytes(path.read_bytes()[:-1])
            with self.assertRaises(ValueError): read_demo(path)

    def test_frame_join_uses_camera_target_not_playback_cursor(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'sample.dem'
            header = bytearray(1072)
            header[:8] = b'HL2DEMO\0'
            struct.pack_into('<ii', header, 8, 4, 24)
            struct.pack_into('<fiii', header, 1056, .1, 6, 1, 0)
            path.write_bytes(header + b'\7')
            frame = dict(frame_index=0, pts=0, demo_tick=6,
                         camera_interpolation=[dict(target_tick=2), None])
            timing = dict(fps=60,time_base_num=1,time_base_den=60,width=256,height=192)
            target = Path(directory) / 'out.jsonl'
            with patch('export_demonstrations.validate', return_value=(timing,[frame],{})):
                export(path,target,Path(directory)/'video.mp4')
            rows = {r['tick']:r for line in target.read_text().splitlines()
                    if (r:=json.loads(line))['type']=='tick'}
            self.assertEqual(len(rows[2]['observations']['screen']['frames']),1)
            self.assertEqual(rows[6]['observations']['screen']['frames'],[])
            self.assertFalse(rows[2]['quality']['training_ready'])


if __name__=='__main__': unittest.main()
