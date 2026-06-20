"""Gymnasium environment for Portal 2.

Registered as ``samuelhautamaki/Portal2-v0``. Communicates with a Source
engine plugin (``portal2_rl_plugin``) over a TCP control socket and a
shared memory frame buffer; see ``docs/protocol.md`` for the wire format.
"""

from __future__ import annotations

from typing import Any

import numpy as np
import gymnasium as gym
from gymnasium import spaces

from portal2_gym.frame_buffer import FrameBuffer
from portal2_gym.launcher import (
    GameLauncher,
    DEFAULT_MAP,
    DEFAULT_WIDTH,
    DEFAULT_HEIGHT,
    DEFAULT_PORT,
)
from portal2_gym.protocol import ControlConnection


# Action vector layout (Box) -- see docs/protocol.md ACTION message.
# Continuous components first, then binary buttons encoded as
# thresholded floats so the whole action space is a single Box.
_CONT_LOW = np.array([-1.0, -1.0, -10.0, -10.0], dtype=np.float32)
_CONT_HIGH = np.array([1.0, 1.0, 10.0, 10.0], dtype=np.float32)
_BUTTON_LOW = np.array([0.0, 0.0, 0.0, 0.0, 0.0], dtype=np.float32)
_BUTTON_HIGH = np.array([1.0, 1.0, 1.0, 1.0, 1.0], dtype=np.float32)
_BUTTON_THRESHOLD = 0.5

# Order: move_forward, move_side, pitch_delta, yaw_delta,
#        jump, duck, use, fire_portal1, fire_portal2
ACTION_LOW = np.concatenate([_CONT_LOW, _BUTTON_LOW])
ACTION_HIGH = np.concatenate([_CONT_HIGH, _BUTTON_HIGH])


class Portal2Env(gym.Env):
    """Portal 2 Gymnasium environment.

    Observation space: a ``Dict`` with
      - ``"screen"``: ``Box(0, 255, (H, W, 3), uint8)`` -- RGB frame.
      - ``"state"``: ``Box(-inf, inf, (16,), float32)`` -- flattened game
        state vector (see :meth:`_state_to_vector`).

    Action space: ``Box(ACTION_LOW, ACTION_HIGH, (9,), float32)``. The
    last 5 components are interpreted as booleans via a 0.5 threshold.

    Parameters
    ----------
    map_name:
        Map to load on reset. Defaults to ``"sp_a2_triple_laser"``.
    width, height:
        Game window / observation resolution.
    port:
        TCP port the plugin's control socket listens on.
    render_mode:
        ``"rgb_array"`` (default, returns the screen frame via
        :meth:`render`) or ``None``.
    launcher_kwargs:
        Extra keyword arguments forwarded to :class:`GameLauncher`
        (e.g. ``steam_root``, ``portal2_dir``, ``portal2_exe``,
        ``has_sar=True`` to also load SourceAutoRecord alongside our
        plugin -- currently used for its ``sar_disable_no_focus_sleep``
        fix, since unfocused Source windows are throttled and
        ``engine_no_focus_sleep`` is not a real Portal 2 cvar).
    connect_timeout:
        Seconds to wait for the plugin's control socket to come up after
        launching the game process.
    """

    metadata = {"render_modes": ["rgb_array"], "render_fps": 66}

    def __init__(
        self,
        map_name: str = DEFAULT_MAP,
        width: int = DEFAULT_WIDTH,
        height: int = DEFAULT_HEIGHT,
        port: int = DEFAULT_PORT,
        render_mode: str | None = "rgb_array",
        launcher_kwargs: dict[str, Any] | None = None,
        connect_timeout: float = 120.0,
        _skip_launch: bool = False,
    ):
        super().__init__()

        self.map_name = map_name
        self.width = width
        self.height = height
        self.port = port
        self.render_mode = render_mode
        self.connect_timeout = connect_timeout
        self._skip_launch = _skip_launch

        self._launcher = GameLauncher(
            width=width,
            height=height,
            port=port,
            default_map=map_name,
            **(launcher_kwargs or {}),
        )

        self.observation_space = spaces.Dict(
            {
                "screen": spaces.Box(
                    low=0, high=255, shape=(height, width, 3), dtype=np.uint8
                ),
                "state": spaces.Box(
                    low=-np.inf, high=np.inf, shape=(18,), dtype=np.float32
                ),
            }
        )
        self.action_space = spaces.Box(
            low=ACTION_LOW, high=ACTION_HIGH, dtype=np.float32
        )

        self._conn: ControlConnection | None = None
        self._frame_buf: FrameBuffer | None = None
        self._last_frame: np.ndarray | None = None
        self._tick: int = 0
        self._connected = False

    # ------------------------------------------------------------------
    # Connection lifecycle
    # ------------------------------------------------------------------

    def _ensure_connected(self) -> None:
        if self._connected:
            return

        if not self._skip_launch and not self._launcher.is_running():
            self._launcher.start()

        self._conn = ControlConnection.connect(
            "127.0.0.1", self.port, timeout=self.connect_timeout
        )
        self._conn.send({"type": "HELLO", "width": self.width, "height": self.height})
        ack = self._conn.recv_type("HELLO_ACK")

        shm_name = ack["shm_name"]
        width = int(ack["width"])
        height = int(ack["height"])
        channels = int(ack.get("channels", 3))

        if (width, height) != (self.width, self.height):
            raise RuntimeError(
                f"Plugin reported resolution {width}x{height}, "
                f"expected {self.width}x{self.height}"
            )

        self._frame_buf = FrameBuffer(shm_name, width, height, channels)
        self._last_frame = np.zeros((height, width, channels), dtype=np.uint8)
        self._connected = True

    # ------------------------------------------------------------------
    # State decoding
    # ------------------------------------------------------------------

    @staticmethod
    def _state_to_vector(state: dict[str, Any]) -> np.ndarray:
        """Flatten the protocol ``state`` dict into a fixed-size float32 vector.

        Layout (18 floats). Portal 2 SP has no health/death mechanic, so
        it is omitted entirely (unlike many other Source-engine RL setups).

          [0:3]   pos (x, y, z)
          [3:6]   vel (x, y, z)
          [6:9]   ang (pitch, yaw, roll)
          [9]     on_ground (0/1)
          [10]    portal1 placed (0/1)
          [11:14] portal1 pos (x, y, z)
          [14]    portal2 placed (0/1)
          [15:18] portal2 pos (x, y, z)

        ``map_finished`` is deliberately excluded from the observation
        vector (it would let a policy "see" termination one step early via
        the obs dict in addition to the `terminated` return value); it is
        still available via ``info["raw_state"]["map_finished"]``.
        """
        pos = state.get("pos", [0.0, 0.0, 0.0])
        vel = state.get("vel", [0.0, 0.0, 0.0])
        ang = state.get("ang", [0.0, 0.0, 0.0])
        p1 = state.get("portal1", {})
        p2 = state.get("portal2", {})
        p1_pos = p1.get("pos", [0.0, 0.0, 0.0])
        p2_pos = p2.get("pos", [0.0, 0.0, 0.0])

        vec = np.array(
            [
                pos[0], pos[1], pos[2],
                vel[0], vel[1], vel[2],
                ang[0], ang[1], ang[2],
                1.0 if state.get("on_ground") else 0.0,
                1.0 if p1.get("placed") else 0.0,
                p1_pos[0], p1_pos[1], p1_pos[2],
                1.0 if p2.get("placed") else 0.0,
                p2_pos[0], p2_pos[1], p2_pos[2],
            ],
            dtype=np.float32,
        )
        return vec

    def _read_observation(self, msg: dict[str, Any]) -> dict[str, Any]:
        state = msg.get("state", {})

        if msg.get("frame_ready", False) and self._frame_buf is not None:
            frame = self._frame_buf.read()
            self._last_frame = frame
        else:
            frame = self._last_frame

        obs = {
            "screen": frame,
            "state": self._state_to_vector(state),
        }
        return obs, state

    # ------------------------------------------------------------------
    # Gymnasium API
    # ------------------------------------------------------------------

    def reset(
        self,
        *,
        seed: int | None = None,
        options: dict[str, Any] | None = None,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        super().reset(seed=seed)

        self._ensure_connected()
        assert self._conn is not None

        map_name = (options or {}).get("map", self.map_name)
        self._conn.send({"type": "RESET", "map": map_name})
        ack = self._conn.recv_type("RESET_ACK", timeout=120.0)

        self._tick = int(ack.get("tick", 0))
        obs, state = self._read_observation(ack)

        info = {"tick": self._tick, "raw_state": state}
        return obs, info

    def step(
        self, action: np.ndarray
    ) -> tuple[dict[str, Any], float, bool, bool, dict[str, Any]]:
        if not self._connected:
            raise RuntimeError("Environment must be reset before calling step()")
        assert self._conn is not None

        action = np.asarray(action, dtype=np.float32)
        action_msg = {
            "type": "ACTION",
            "move_forward": float(action[0]),
            "move_side": float(action[1]),
            "pitch_delta": float(action[2]),
            "yaw_delta": float(action[3]),
            "jump": bool(action[4] > _BUTTON_THRESHOLD),
            "duck": bool(action[5] > _BUTTON_THRESHOLD),
            "use": bool(action[6] > _BUTTON_THRESHOLD),
            "fire_portal1": bool(action[7] > _BUTTON_THRESHOLD),
            "fire_portal2": bool(action[8] > _BUTTON_THRESHOLD),
        }
        self._conn.send(action_msg)

        msg = self._conn.recv_type("TICK")
        self._tick = int(msg.get("tick", self._tick + 1))
        obs, state = self._read_observation(msg)

        # Reward is computed entirely in Python / by wrappers; base env
        # returns 0.0 and exposes raw state for reward-shaping wrappers.
        # Portal 2 SP has no health/death mechanic, so map_finished
        # (reaching the level's point_changelevel / exit elevator) is the
        # only built-in termination signal.
        reward = 0.0
        terminated = bool(state.get("map_finished", False))
        truncated = False

        info = {"tick": self._tick, "raw_state": state}
        return obs, reward, terminated, truncated, info

    def render(self):
        if self.render_mode == "rgb_array":
            return self._last_frame
        return None

    def close(self) -> None:
        if self._conn is not None:
            try:
                self._conn.send({"type": "CLOSE"})
            except Exception:
                pass
            self._conn.close()
            self._conn = None

        if self._frame_buf is not None:
            self._frame_buf.close()
            self._frame_buf = None

        if not self._skip_launch:
            self._launcher.stop()
        self._connected = False
