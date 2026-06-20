"""Newline-delimited JSON control socket client.

See ``docs/protocol.md`` for the full message specification. This module
implements only the Python (client) side: connect, send a JSON message,
and receive the next JSON message, with line buffering since TCP does not
preserve message boundaries.
"""

from __future__ import annotations

import json
import socket
import time
from typing import Any


class ProtocolError(RuntimeError):
    """Raised when the plugin sends a malformed or unexpected message."""


class ControlConnection:
    """A connected TCP socket to the plugin's control server."""

    def __init__(self, sock: socket.socket):
        self._sock = sock
        self._sock.settimeout(None)
        self._recv_buf = b""

    @classmethod
    def connect(cls, host: str, port: int, timeout: float = 30.0,
                 retry_interval: float = 0.5) -> "ControlConnection":
        """Connect to the plugin's control server, retrying until ready.

        The plugin opens its listen socket during ``Load()``, but the game
        process takes time to start up, so we retry connection attempts
        until ``timeout`` seconds have elapsed.
        """
        deadline = time.monotonic() + timeout
        last_err: Exception | None = None
        while time.monotonic() < deadline:
            try:
                sock = socket.create_connection((host, port), timeout=5.0)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                return cls(sock)
            except OSError as e:
                last_err = e
                time.sleep(retry_interval)
        raise ConnectionError(
            f"Could not connect to plugin control socket at {host}:{port} "
            f"within {timeout}s"
        ) from last_err

    def send(self, message: dict[str, Any]) -> None:
        data = (json.dumps(message) + "\n").encode("utf-8")
        self._sock.sendall(data)

    def recv(self, timeout: float | None = None) -> dict[str, Any]:
        """Receive the next complete JSON message (blocking by default)."""
        self._sock.settimeout(timeout)
        try:
            while b"\n" not in self._recv_buf:
                chunk = self._sock.recv(65536)
                if not chunk:
                    raise ConnectionError("Plugin closed the control socket")
                self._recv_buf += chunk
        finally:
            self._sock.settimeout(None)

        line, _, self._recv_buf = self._recv_buf.partition(b"\n")
        try:
            return json.loads(line.decode("utf-8"))
        except json.JSONDecodeError as e:
            raise ProtocolError(f"Malformed message from plugin: {line!r}") from e

    def recv_type(self, expected_type: str, timeout: float | None = None) -> dict[str, Any]:
        """Receive a message and assert its ``type`` field."""
        msg = self.recv(timeout=timeout)
        if msg.get("type") != expected_type:
            raise ProtocolError(
                f"Expected message of type '{expected_type}', got {msg!r}"
            )
        return msg

    def close(self) -> None:
        try:
            self._sock.close()
        except Exception:
            pass
