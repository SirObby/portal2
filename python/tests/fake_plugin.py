"""A minimal fake plugin server for testing portal2_gym.env.Portal2Env
without requiring Portal 2 or the C++ plugin to be present.

Implements just enough of docs/protocol.md to drive reset()/step() through
their full code paths: HELLO/HELLO_ACK handshake, shared memory frame
buffer, RESET/RESET_ACK, and TICK/ACTION lockstep.
"""

from __future__ import annotations

import json
import socket
import threading
from multiprocessing import shared_memory


class FakePlugin:
    def __init__(self, port: int, width: int, height: int, shm_name: str = "p2rl_test"):
        self.port = port
        self.width = width
        self.height = height
        self.channels = 3
        self.shm_name = shm_name
        self._shm = shared_memory.SharedMemory(
            name=shm_name, create=True, size=width * height * 3
        )
        self._tick = 0
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind(("127.0.0.1", port))
        self._server.listen(1)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._stop = False

    def start(self):
        self._thread.start()

    def _send(self, conn, msg):
        conn.sendall((json.dumps(msg) + "\n").encode())

    def _recv(self, conn, buf: bytearray):
        while b"\n" not in buf:
            chunk = conn.recv(65536)
            if not chunk:
                raise ConnectionError
            buf.extend(chunk)
        line, _, rest = bytes(buf).partition(b"\n")
        buf.clear()
        buf.extend(rest)
        return json.loads(line.decode())

    def _fill_frame(self, value: int):
        mv = self._shm.buf
        for i in range(self.width * self.height * self.channels):
            mv[i] = value % 256

    def _state(self, finished=False):
        return {
            "pos": [1.0, 2.0, 3.0],
            "vel": [0.0, 0.0, 0.0],
            "ang": [0.0, 0.0, 0.0],
            "on_ground": True,
            "portal1": {"placed": False, "pos": [0, 0, 0], "normal": [0, 0, 1]},
            "portal2": {"placed": False, "pos": [0, 0, 0], "normal": [0, 0, 1]},
            "map": "sp_a2_triple_laser",
            "map_finished": finished,
            "in_loading": False,
        }

    def _run(self):
        conn, _ = self._server.accept()
        buf = bytearray()
        try:
            hello = self._recv(conn, buf)
            assert hello["type"] == "HELLO"
            self._send(conn, {
                "type": "HELLO_ACK",
                "shm_name": self.shm_name,
                "width": self.width,
                "height": self.height,
                "channels": self.channels,
                "tick_interval": 0.015,
            })

            while True:
                msg = self._recv(conn, buf)
                if msg["type"] == "RESET":
                    self._tick = 0
                    self._fill_frame(self._tick)
                    self._send(conn, {
                        "type": "RESET_ACK",
                        "tick": self._tick,
                        "frame_ready": True,
                        "state": self._state(),
                    })
                elif msg["type"] == "ACTION":
                    self._tick += 1
                    self._fill_frame(self._tick)
                    finished = self._tick >= 5
                    self._send(conn, {
                        "type": "TICK",
                        "tick": self._tick,
                        "frame_ready": True,
                        "state": self._state(finished=finished),
                    })
                elif msg["type"] == "CLOSE":
                    break
        except (ConnectionError, OSError):
            pass
        finally:
            conn.close()

    def close(self):
        try:
            self._server.close()
        except Exception:
            pass
        try:
            self._shm.close()
            self._shm.unlink()
        except Exception:
            pass
