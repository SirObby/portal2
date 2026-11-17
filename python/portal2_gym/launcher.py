"""Launches and manages the Portal 2 game process with the RL plugin loaded."""

from __future__ import annotations

import os
import platform
import shlex
import subprocess
from pathlib import Path


DEFAULT_MAP = "sp_a2_triple_laser"
DEFAULT_WIDTH = 256
DEFAULT_HEIGHT = 192
DEFAULT_PORT = 27182
PLUGIN_NAME = "portal2_rl_plugin"


class GameLauncher:
    """Starts the Portal 2 process configured to load the RL plugin.

    On Linux, the game is launched via the Steam runtime wrapper script,
    matching::

        $HOME/.local/share/Steam/ubuntu12_32/steam-runtime/run.sh \\
            $HOME/.steam/steam/steamapps/common/Portal 2/portal2.sh \\
            -game portal2 -steam -vulkan -windowed -w <W> -h <H> -novid \\
            +map <map> +plugin_load <plugin> +portal2_rl_port <port>

    On Windows, ``hl2.exe`` (Portal 2's binary) is launched directly with
    the equivalent ``-game portal2`` flags; the user must supply the path
    via ``portal2_exe`` since Steam's Windows layout has no equivalent of
    the Linux runtime wrapper script.
    """

    def __init__(
        self,
        width: int = DEFAULT_WIDTH,
        height: int = DEFAULT_HEIGHT,
        port: int = DEFAULT_PORT,
        default_map: str = DEFAULT_MAP,
        steam_root: str | None = None,
        portal2_dir: str | None = None,
        portal2_exe: str | None = None,
        extra_args: list[str] | None = None,
        has_sar: bool = False,
    ):
        self.width = width
        self.height = height
        self.port = port
        self.default_map = default_map
        self.extra_args = extra_args or []
        self.has_sar = has_sar

        home = Path.home()
        self.steam_root = Path(steam_root) if steam_root else (
            home / ".local" / "share" / "Steam"
        )
        self.portal2_dir = Path(portal2_dir) if portal2_dir else (
            home / ".steam" / "steam" / "steamapps" / "common" / "Portal 2"
        )
        self.portal2_exe = portal2_exe

        self._proc: subprocess.Popen | None = None

    def _common_options(self) -> list[str]:
        """Flags/console commands shared by both platforms' launch
        commands: plugin loading and HUD/caption suppression.

        Note: SourceAutoRecord (has_sar=True) is intentionally NOT loaded
        here via "+plugin_load sar" -- that does not reliably take effect
        from the launch command line (confirmed: SAR fails to load this
        way even though our own plugin loads fine via the same
        mechanism). Instead, when has_sar=True, our own plugin issues
        "plugin_load sar" itself via IVEngineServer::ClientCommand once
        it's safely running (see RLBridge::Start's has_sar handling in
        the C++ side), which is a console command path already proven to
        work reliably.
        """
        opts = [
            "+map", self.default_map,
            "+plugin_load", PLUGIN_NAME,
            "+portal2_rl_port", str(self.port),
            "+cl_drawhud", "0",
            "+crosshair", "0",
            "+closecaption", "0",
            "+viewmodel_offset_x", "5000",
            "+hidehud", "7",
        ]
        if self.has_sar:
            # Tells our own plugin (via the same command-line port-style
            # parsing used for portal2_rl_port) to load SAR itself after
            # Load() completes, since +plugin_load sar doesn't take
            # effect reliably when placed directly on the launch line.
            opts += ["+portal2_rl_has_sar", "1"]
        return opts

    def _build_command_linux(self) -> list[str]:
        run_sh = self.steam_root / "ubuntu12_32" / "steam-runtime" / "run.sh"
        portal2_sh = self.portal2_dir / "portal2.sh"

        cmd = [
            str(run_sh),
            str(portal2_sh),
            "-game", "portal2",
            "-steam",
            "-vulkan",
            "-windowed",
            "-w", str(self.width),
            "-h", str(self.height),
            "-novid",
        ]
        cmd += self._common_options()
        cmd.extend(self.extra_args)
        return cmd

    def _build_command_windows(self) -> list[str]:
        if not self.portal2_exe:
            raise RuntimeError(
                "On Windows, 'portal2_exe' must be set to the path of "
                "hl2.exe inside the Portal 2 install directory."
            )
        cmd = [
            self.portal2_exe,
            "-game", "portal2",
            "-steam",
            "-windowed",
            "-w", str(self.width),
            "-h", str(self.height),
            "-novid",
        ]
        cmd += self._common_options()
        cmd.extend(self.extra_args)
        return cmd


    def build_command(self) -> list[str]:
        system = platform.system()
        if system == "Linux":
            return self._build_command_linux()
        elif system == "Windows":
            return self._build_command_windows()
        else:
            raise RuntimeError(f"Unsupported platform: {system}")

    def start(self) -> subprocess.Popen:
        if self._proc is not None and self._proc.poll() is None:
            return self._proc

        cmd = self.build_command()
        cwd = str(self.portal2_dir) if Path(self.portal2_dir).exists() else None

        self._proc = subprocess.Popen(
            cmd,
            cwd=cwd,
        )
        return self._proc

    def is_running(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def stop(self, timeout: float = 10.0) -> None:
        if self._proc is None:
            return
        if self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
        self._proc = None

    def command_str(self) -> str:
        """Return the launch command as a shell-quoted string (for logging)."""
        return " ".join(shlex.quote(c) for c in self.build_command())
