"""Portal2-RL Gymnasium environment package.

Registers ``samuelhautamaki/Portal2-v0`` with Gymnasium on import.
"""

from gymnasium.envs.registration import register

from portal2_gym.env import Portal2Env

__all__ = ["Portal2Env"]
__version__ = "0.1.0"

register(
    id="samuelhautamaki/Portal2-v0",
    entry_point="portal2_gym.env:Portal2Env",
    max_episode_steps=None,  # caller can wrap with TimeLimit if desired
)
