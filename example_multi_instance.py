"""
example_multi_instance.py — Portal2-RL example: run multiple environment
instances in parallel.

Each instance launches its own Portal 2 process on a different port, so
they run fully independently. Useful for testing that the plugin/env
supports concurrent instances (e.g. for vectorized training later).

Run from the project root:
    cd python && uv run ../example_multi_instance.py [N]

N = number of instances (default 2).
"""

import sys
import threading
import time

import numpy as np
import gymnasium as gym
import portal2_gym  # registers samuelhautamaki/Portal2-v0


N = int(sys.argv[1]) if len(sys.argv) > 1 else 2
BASE_PORT = 27182


def run_instance(index: int):
    port = BASE_PORT + index
    tag = f"[env{index} :{port}]"
    print(f"{tag} starting...")

    env = gym.make(
        "samuelhautamaki/Portal2-v0",
        port=port,
        width=256,
        height=192,
    )

    try:
        obs, info = env.reset()
        print(f"{tag} connected, map={info['raw_state']['map']}")

        step = 0
        while step < 2000:
            action = env.action_space.sample()
            # Tame the random sampling a bit so it's not 100% chaotic.
            action[2:4] *= 0.3  # smaller look deltas
            action[4:] = (action[4:] > 1.5).astype(np.float32)  # rarely press buttons

            obs, reward, terminated, truncated, info = env.step(action)
            step += 1

            if step % 200 == 0:
                print(f"{tag} tick={info['tick']}")

            if terminated or truncated:
                print(f"{tag} episode ended, resetting")
                obs, info = env.reset()

    except Exception as e:
        print(f"{tag} ERROR: {e}")
    finally:
        env.close()
        print(f"{tag} closed")


def main():
    print(f"Launching {N} instances on ports {BASE_PORT}..{BASE_PORT + N - 1}")

    threads = []
    for i in range(N):
        t = threading.Thread(target=run_instance, args=(i,), daemon=True)
        threads.append(t)
        t.start()
        time.sleep(2.0)  # stagger launches a bit to ease load on Steam/disk

    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        print("\nInterrupted, instances will close as their threads exit.")


if __name__ == "__main__":
    main()
