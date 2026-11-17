import json
from pathlib import Path
import tempfile
import unittest
from copy import deepcopy
from demo_capture import read_capture,state_vector
from prepare_demo_folder import console_value,split_folder_capture


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)/'capture.jsonl'
        self.state=dict(pos=[1,2,3],vel=[4,5,6],ang=[7,8,9],on_ground=True,
            portal1=dict(placed=True,pos=[10,11,12]),portal2=dict(placed=False,pos=[0,0,0]),portals_valid=True)
        self.cmd=dict(command_number=10,tick_count=20,buttons=3,view_angles=[1,2,3],forward_move=175,side_move=0,up_move=0)
        self.records=[dict(slot=0,command_index=10,usercmd=self.cmd)]
        self.sample=dict(type='command',index=0,slot=0,sequence=10,command_number=10,
            command_tick=20,buttons=3,view_angles=[1,2,3],movement=[175,0,0],state=self.state,
            skipping=False,paused=False,stage='before_decode_usercmd',playback_tick=5)

    def read(self,samples=None,sha='abc',complete=True):
        samples=[self.sample] if samples is None else samples
        rows=[dict(type='header',schema='portal2-client-demo-capture-v1',demo_sha256=sha),*samples,
              dict(type='end',complete=complete,command_count=len(samples))]
        self.path.write_text(''.join(json.dumps(r)+'\n' for r in rows))
        return read_capture(self.path,'abc',self.records)

    def test_full_state_layout(self):
        values,valid=state_vector(self.state)
        self.assertEqual(values,[1,2,3,4,5,6,7,8,9,1,1,10,11,12,0,0,0,0])
        self.assertTrue(all(valid))

    def test_capture_binds_exact_command(self):
        index,report=self.read()
        self.assertEqual(index[(0,10)][0]['state'][:3],[1,2,3])
        self.assertTrue(report['action_decoding_verified'])
        self.assertFalse(report['causal_simulation_alignment_verified'])

    def test_identity_mismatch_rejected(self):
        self.sample['buttons']=4
        with self.assertRaisesRegex(ValueError,'identity mismatch'): self.read()

    def test_demo_hash_and_incomplete_rejected(self):
        with self.assertRaisesRegex(ValueError,'hash mismatch'): self.read(sha='bad')
        with self.assertRaisesRegex(ValueError,'Incomplete'): self.read(complete=False)

    def test_playback_restart_selects_final_pass(self):
        old=deepcopy(self.sample); new=deepcopy(self.sample)
        new['index']=1; new['state']['pos']=[50,60,70]
        index,report=self.read([old,new])
        self.assertEqual(report['final_pass_start'],1)
        self.assertEqual(index[(0,10)][0]['state'][:3],[50,60,70])

    def test_skipping_not_admitted(self):
        self.sample['skipping']=True
        index,report=self.read()
        self.assertFalse(index)
        self.assertEqual(report['excluded_paused_or_skipping'],1)

    def test_missing_portal_fields_stay_missing(self):
        self.state['portals_valid']=False
        values,valid=state_vector(self.state)
        self.assertEqual(values[10:],[None]*8)
        self.assertFalse(any(valid[10:]))

    def test_reject_console_injection(self):
        for value in ['a;b','a\nb','a"b']:
            with self.assertRaises(ValueError): console_value(value)
        self.assertEqual(console_value('/a path/file'), '"/a path/file"')

    def test_folder_journal_splits_by_demo_identity(self):
        root=Path(self.temp.name)
        first=root/'one.dem'; second=root/'two.dem'
        first.write_bytes(b'first demo'); second.write_bytes(b'second demo')
        samples=[dict(self.sample,index=0,demo_name=str(first)),
                 dict(self.sample,index=1,demo_name=str(second)),
                 dict(self.sample,index=2,demo_name=str(first))]
        self.path.write_text(''.join(json.dumps(row)+'\n' for row in [
            dict(type='header',schema='portal2-client-demo-capture-v1',demo_sha256=''),
            *samples,dict(type='end',complete=True,command_count=3)]))
        split_folder_capture(self.path,[first,second],root/'episodes')
        rows=[json.loads(line) for line in (root/'episodes/one/capture.jsonl').read_text().splitlines()]
        self.assertEqual(rows[-1]['command_count'],2)
        self.assertEqual([s['index'] for s in rows[1:-1]],[0,1])
        self.assertEqual([s['source_native_index'] for s in rows[1:-1]],[0,2])
        with self.assertRaisesRegex(ValueError,'unexpected demo'):
            split_folder_capture(self.path,[first],root/'bad')


if __name__=='__main__': unittest.main()
