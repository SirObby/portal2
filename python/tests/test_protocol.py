import json
import socket
import threading

import numpy as np
import pytest

from portal2_gym.env import Portal2Env
from portal2_gym.protocol import ControlConnection, ProtocolError


SAMPLE_STATE = {
    "pos": [100.0, 200.0, 64.0],
    "vel": [10.0, 0.0, 0.0],
    "ang": [5.0, 90.0, 0.0],
    "on_ground": True,
    "portal1": {"placed": True, "pos": [10.0, 20.0, 30.0], "normal": [0, 0, 1]},
    "portal2": {"placed": False, "pos": [0, 0, 0], "normal": [0, 0, 0]},
    "map": "sp_a2_triple_laser",
    "map_finished": False,
    "in_loading": False,
}


def test_state_to_vector_shape_and_values():
    vec = Portal2Env._state_to_vector(SAMPLE_STATE)
    assert vec.shape == (18,)
    assert vec.dtype == np.float32
    assert vec[0:3].tolist() == [100.0, 200.0, 64.0]
    assert vec[9] == 1.0  # on_ground
    assert vec[10] == 1.0  # portal1 placed
    assert vec[11:14].tolist() == [10.0, 20.0, 30.0]
    assert vec[14] == 0.0  # portal2 placed


def test_state_to_vector_handles_missing_fields():
    vec = Portal2Env._state_to_vector({})
    assert vec.shape == (18,)
    assert np.all(np.isfinite(vec))


def _make_pair():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]

    client = socket.create_connection(("127.0.0.1", port))
    conn, _ = server.accept()

    server.close()
    return ControlConnection(conn), client


def test_control_connection_send_recv():
    a, b_sock = _make_pair()
    try:
        a.send({"type": "HELLO_ACK", "shm_name": "/x", "width": 4, "height": 4})
        line = b_sock.recv(4096)
        msg = json.loads(line.decode().strip())
        assert msg["type"] == "HELLO_ACK"
        assert msg["shm_name"] == "/x"
    finally:
        a.close()
        b_sock.close()


def test_control_connection_recv_partial_then_complete():
    a, b_sock = _make_pair()
    try:
        payload = json.dumps({"type": "TICK", "tick": 1, "frame_ready": False, "state": {}})
        data = (payload + "\n").encode()
        # send in two chunks to test buffering
        b_sock.sendall(data[:5])
        b_sock.sendall(data[5:])
        msg = a.recv()
        assert msg["type"] == "TICK"
        assert msg["tick"] == 1
    finally:
        a.close()
        b_sock.close()


def test_recv_type_mismatch_raises():
    a, b_sock = _make_pair()
    try:
        b_sock.sendall((json.dumps({"type": "TICK"}) + "\n").encode())
        with pytest.raises(ProtocolError):
            a.recv_type("HELLO_ACK")
    finally:
        a.close()
        b_sock.close()
