import struct
from pathlib import Path
from unittest.mock import Mock
import pytest
from portal2_gym.demo import DemoRecorder
from portal2_gym.env import Portal2Env


def write_header(path,map_name='sp_a2_triple_laser',ticks=12):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_bytes(struct.pack('<8sii260s260s260s260sfiii',b'HL2DEMO\0',4,24,b'server',b'client',map_name.encode(),b'portal2',ticks/60,ticks,ticks,0))


def test_demo_finalization_copies_hashes_and_validates(tmp_path):
    recorder=DemoRecorder(tmp_path/'out',tmp_path/'game',12345)
    recorder.socket=Mock()
    pending=recorder.start('sp_a2_triple_laser')
    write_header(recorder.source)
    flush=Mock()
    result=recorder.finish(flush)
    assert flush.call_count==5
    assert result['source_demo_verified']
    assert Path(pending).read_bytes()==Path(result['demo_game_copy']).read_bytes()
    assert len(result['demo_sha256'])==64
    assert recorder.finish(flush) is None
    recorder.close()


def test_unfinalized_or_wrong_map_demo_is_rejected(tmp_path):
    recorder=DemoRecorder(tmp_path/'out',tmp_path/'game',12345)
    recorder.socket=Mock();recorder.start('sp_a2_triple_laser')
    write_header(recorder.source,map_name='wrong')
    with pytest.raises(RuntimeError,match='finalize'):recorder.finish(lambda:None)
    assert recorder.source is not None
    assert not list((tmp_path/'out').glob('*.dem'))


def test_env_demo_lifecycle_and_neutral_flush(tmp_path):
    env=Portal2Env(_skip_launch=True,record_demo=True,demo_dir=str(tmp_path))
    env._conn=Mock();env._connected=True
    recorder=env._demo_recorder
    recorder.source=tmp_path/'game.dem';recorder.finish=Mock(return_value={'demo':'owned.dem'})
    recorder.finish.side_effect=lambda flush: (flush(),{'demo':'owned.dem'})[1]
    env._episode_done=False
    assert env.finish_demo()=={'demo':'owned.dem'}
    assert env._conn.send.call_args.args[0]['type']=='ACTION'
    assert env._conn.send.call_args.args[0]['jump'] is False
    assert env._episode_done
    recorder.source=None
    env.close()


def test_demo_port_reuses_launcher_console_and_rejects_conflicts(tmp_path):
    env=Portal2Env(_skip_launch=True,record_demo=True,demo_dir=str(tmp_path),launcher_kwargs={'extra_args':['-netconport','28001']})
    assert env._demo_recorder.port==28001
    assert env._launcher.extra_args.count('-netconport')==1
    env.close()
    with pytest.raises(ValueError,match='differs'):
        Portal2Env(record_demo=True,demo_dir=str(tmp_path),demo_port=28002,launcher_kwargs={'extra_args':['-netconport','28001']})


def test_auto_demo_on_native_terminal_preserves_policy_observation(tmp_path):
    import socket
    from tests.fake_plugin import FakePlugin
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    plugin=FakePlugin(port,8,8,shm_name=f'p2rl_demo_test_{port}')
    plugin.start()
    env=Portal2Env(width=8,height=8,port=port,_skip_launch=True,record_demo=True,
                   demo_dir=str(tmp_path/'out'),launcher_kwargs={'portal2_dir':str(tmp_path/'game')})
    env._demo_recorder.socket=Mock()
    try:
        _,info=env.reset()
        assert 'demo_pending' in info
        write_header(env._demo_recorder.source,ticks=5)
        import numpy as np
        for _ in range(5):
            obs,_,terminated,_,info=env.step(np.zeros(9,np.float32))
        assert terminated and info['source_demo_verified']
        assert info['tick']==5 and env._tick==5
        assert np.all(obs['screen']==5)
        assert Path(info['demo']).is_file()
        assert Path(info['demo']).with_suffix('.json').is_file()
        with pytest.raises(RuntimeError,match='reset'):
            env.step(np.zeros(9,np.float32))
    finally:
        env.close();plugin.close()


def test_challenge_termination_is_distinct_from_map_transition():
    import numpy as np
    env=Portal2Env(_skip_launch=True)
    env._conn=Mock();env._connected=True;env.challenge_requested=True
    env._last_frame=np.zeros((env.height,env.width,3),np.uint8)
    env._conn.recv_type.return_value=dict(tick=1,frame_ready=False,challenge_finished=True,state={'map_finished':False})
    try:
        _,_,terminated,_,info=env.step(np.zeros(9,np.float32))
        assert terminated and info['challenge_finished']
        assert not info['map_transition_finished']
    finally:
        env.close()
