"""
example_jump.py — Portal2-RL example: agent that jumps constantly.

Run from the project root:
    cd python && uv run ../example_jump.py

The game will launch automatically, load sp_a2_triple_laser, and the
agent will jump every tick while walking forward. Watch the game window
to see the agent bouncing around.

Press Ctrl-C to stop. The game process will be killed automatically.
"""

import numpy as np
import gymnasium as gym
import portal2_gym  # registers samuelhautamaki/Portal2-v0

env = gym.make(
    "samuelhautamaki/Portal2-v0",
    render_mode=None,  # set to "rgb_array" if you want env.render() to work
    launcher_kwargs={"has_sar": True}
)

print("Waiting for Portal 2 to start and connect...")
obs, info = env.reset()
print(f"Connected! Map: {info['raw_state']['map']}")
print(f"Player pos: {info['raw_state']['pos']}")
print(f"Tick: {info['tick']}")
print()

# Action layout (9 floats):
#   [0] move_forward  [-1, 1]
#   [1] move_side     [-1, 1]
#   [2] pitch_delta   degrees/tick
#   [3] yaw_delta     degrees/tick
#   [4] jump          >0.5 = pressed
#   [5] duck          >0.5 = pressed
#   [6] use           >0.5 = pressed
#   [7] fire_portal1  >0.5 = pressed (blue)
#   [8] fire_portal2  >0.5 = pressed (orange)

try:
    step = 0
    jump_phase = 0  # spaces out jump presses so we can see distinct hops
    while True:
        # Walk forward and jump periodically. The plugin turns every
        # jump=true into exactly one clean +jump/-jump pulse per tick
        # it's sent (Portal 2 has no auto-bunnyhop), so we still space
        # presses out a bit just to make the hops visually distinct
        # rather than jumping every single tick.
        action = np.zeros(env.action_space.shape, dtype=np.float32)
        action[0] = 1.0   # move_forward

        jump_phase = (jump_phase + 1) % 20
        action[4] = 1.0 if jump_phase < 2 else 0.0  # jump for 2 ticks, pause for 18

        # Slowly rotate to the right so we don't just run into a wall.
        action[3] = 0.5   # yaw_delta (degrees per tick)

        obs, reward, terminated, truncated, info = env.step(action)
        step += 1

        if step % 66 == 0:  # ~once per second at 66 ticks/s
            state = info["raw_state"]
            print(
                f"tick={info['tick']:5d}  "
                f"pos=({state['pos'][0]:7.0f}, {state['pos'][1]:7.0f}, {state['pos'][2]:5.0f})  "
                f"on_ground={state['on_ground']}  "
                f"map_finished={state['map_finished']}"
            )

        if terminated or truncated:
            print(f"\nEpisode ended at tick {info['tick']} "
                  f"({'map finished!' if terminated else 'truncated'})")
            print("Resetting...")
            obs, info = env.reset()
            print(f"New episode started. Map: {info['raw_state']['map']}")
            step = 0

except KeyboardInterrupt:
    print("\nStopping.")
finally:
    env.close()
    print("Done.")
