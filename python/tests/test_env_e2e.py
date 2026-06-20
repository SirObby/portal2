import socket
import time

import numpy as np

from portal2_gym.env import Portal2Env
from tests.fake_plugin import FakePlugin


def _free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def test_reset_and_step_against_fake_plugin():
    port = _free_port()
    width, height = 8, 8
    plugin = FakePlugin(port, width, height, shm_name=f"p2rl_test_{port}")
    plugin.start()

    env = Portal2Env(
        width=width,
        height=height,
        port=port,
        connect_timeout=10.0,
        _skip_launch=True,
    )

    try:
        obs, info = env.reset()
        assert obs["screen"].shape == (height, width, 3)
        assert obs["state"].shape == (18,)
        assert info["tick"] == 0
        assert np.all(obs["screen"] == 0)

        action = env.action_space.sample()
        action[:] = 0.0

        terminated = False
        for expected_tick in range(1, 6):
            obs, reward, terminated, truncated, info = env.step(action)
            assert info["tick"] == expected_tick
            assert np.all(obs["screen"] == (expected_tick % 256))
            if terminated:
                break

        assert terminated  # map_finished triggers at tick 5
    finally:
        env.close()
        plugin.close()
