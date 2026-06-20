"""Cross-platform shared memory reader for the rendered game frame.

The C++ plugin writes raw RGB24 pixel data into a shared memory segment.
On Linux this is a POSIX shm object (``/dev/shm/<name>``), opened via
``multiprocessing.shared_memory.SharedMemory`` which wraps ``shm_open`` +
``mmap``. On Windows the same stdlib class wraps ``CreateFileMapping`` /
``MapViewOfFile``, using the name as given (the plugin is responsible for
using a name compatible with both, i.e. avoiding leading slashes being
required on Windows -- ``SharedMemory`` handles this normalization).
"""

from __future__ import annotations

import numpy as np
from multiprocessing import shared_memory


class FrameBuffer:
    """Read-only view onto a shared memory RGB frame buffer."""

    def __init__(self, shm_name: str, width: int, height: int, channels: int = 3):
        self.width = width
        self.height = height
        self.channels = channels
        self.nbytes = width * height * channels

        # SharedMemory on POSIX expects a name without a leading '/'
        # in some Python versions but accepts it in others; normalize by
        # stripping a single leading '/' if present, since the plugin
        # may send either form.
        name = shm_name.lstrip("/")

        self._shm = shared_memory.SharedMemory(name=name, create=False)
        if self._shm.size < self.nbytes:
            raise RuntimeError(
                f"Shared memory segment '{shm_name}' is too small: "
                f"{self._shm.size} bytes, need {self.nbytes}"
            )

    def read(self) -> np.ndarray:
        """Return a copy of the current frame as an (H, W, C) uint8 array."""
        buf = np.frombuffer(self._shm.buf, dtype=np.uint8, count=self.nbytes)
        frame = buf.reshape((self.height, self.width, self.channels))
        return frame.copy()

    def close(self) -> None:
        try:
            self._shm.close()
        except Exception:
            pass
