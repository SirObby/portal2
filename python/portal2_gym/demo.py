"""Owned-game Source demo lifecycle via its loopback netconsole."""
from pathlib import Path
import hashlib
import json
import shutil
import socket
import struct
import uuid


def demo_header(path):
    with Path(path).open('rb') as stream:
        raw = stream.read(1072)
    if len(raw) != 1072 or raw[:8] != b'HL2DEMO\0':
        raise ValueError('Invalid recorded Source demo header')
    row = struct.unpack('<8sii260s260s260s260sfiii', raw)
    return dict(demo_protocol=row[1], network_protocol=row[2],
                map=row[5].split(b'\0', 1)[0].decode(), seconds=row[7],
                ticks=row[8], frames=row[9], signon_bytes=row[10])


class DemoRecorder:
    def __init__(self, directory, game_directory, port):
        self.directory = Path(directory).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.game_directory = Path(game_directory)
        self.port = port
        self.socket = None
        self.source = None
        self.map_name = None

    def start(self, map_name):
        if self.source is not None:
            raise RuntimeError('Previous Source demo has not been finalized')
        if self.socket is None:
            self.socket = socket.create_connection(('127.0.0.1', self.port), timeout=5)
        basename = 'p2rl_' + uuid.uuid4().hex
        self.source = self.game_directory / 'portal2' / (basename + '.dem')
        self.map_name = map_name
        self.socket.sendall(('record ' + basename + '\n').encode())
        return str(self.directory / self.source.name)

    def finish(self, flush):
        if self.source is None:
            return None
        self.socket.sendall(b'stop\n')
        # Source queues console commands while the lockstep bridge is blocked.
        # These are post-episode neutral frames, never policy transitions.
        for _ in range(5):
            flush()
        header = demo_header(self.source)
        if header['map'] != self.map_name or header['ticks'] <= 0 or header['seconds'] <= 0:
            raise RuntimeError('Source demo did not finalize correctly')
        destination = self.directory / self.source.name
        if destination.exists():
            raise FileExistsError(destination)
        shutil.copy2(self.source, destination)
        digest = hashlib.sha256(destination.read_bytes()).hexdigest()
        result = dict(demo=str(destination), demo_sha256=digest, demo_header=header,
                      demo_game_copy=str(self.source), source_demo_verified=True,
                      demo_post_episode_flush_steps=5)
        destination.with_suffix('.json').write_text(json.dumps(result, indent=2)+'\n')
        self.source = None
        return result

    def close(self):
        if self.socket is not None:
            self.socket.close()
            self.socket = None
