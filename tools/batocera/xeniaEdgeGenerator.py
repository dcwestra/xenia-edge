"""Batocera configgen generator for the native Linux build of Xenia Edge.

Batocera's own Xenia support runs the Windows builds under Wine. This runs
xenia_edge natively with Vulkan, as the "xenia-edge" emulator of the xbox360
system. emulatorlauncher-edge registers it, so Batocera's launcher still sets
up controllers, hotkeys, the power mode and the video mode around it.

Layout under /userdata, all of it persistent across Batocera updates:
  system/xenia-edge/xenia_edge       the emulator
  system/configs/xenia-edge/         storage root: xenia-edge.config.toml,
                                     config/<title id>.config.toml, patches/
  system/cache/xenia-edge/           shader and pipeline caches
  saves/xbox360/                     content (profiles, saves), shared with
                                     the Wine builds
  system/logs/xenia-edge/<game>.log  the last session of each game
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import TYPE_CHECKING

from configgen import Command
from configgen.batoceraPaths import CACHE, CONFIGS, HOME, LOGS, SAVES, configure_emulator, mkdir_if_not_exists
from configgen.controller import generate_sdl_game_controller_config
from configgen.generators.Generator import Generator

if TYPE_CHECKING:
    from configgen.types import HotkeysContext

_logger = logging.getLogger(__name__)

XENIA_EDGE_DIR = HOME / 'xenia-edge'
XENIA_EDGE_BIN = XENIA_EDGE_DIR / 'xenia_edge'
XENIA_EDGE_STORAGE = CONFIGS / 'xenia-edge'
XENIA_EDGE_CACHE = CACHE / 'xenia-edge'
XENIA_EDGE_CONTENT = SAVES / 'xbox360'
XENIA_EDGE_LOGS = LOGS / 'xenia-edge'


def _flag(value: bool) -> str:
    return 'true' if value else 'false'


class XeniaEdgeGenerator(Generator):

    def getHotkeysContext(self) -> HotkeysContext:
        return {
            "name": "xenia-edge",
            "keys": {"exit": ["KEY_LEFTALT", "KEY_F4"]},
        }

    def getMouseMode(self, config, rom):
        return True

    def getInGameRatio(self, config, gameResolution, rom):
        return 16 / 9

    def executionDirectory(self, config, rom):
        return XENIA_EDGE_DIR

    @staticmethod
    def _resolve_playlist(rom: Path) -> Path:
        # A .xbox360 file names a disc installation or XBLA title, relative to
        # the file, on its first line - as with Batocera's Wine Xenia.
        if rom.suffix != '.xbox360':
            return rom
        with rom.open() as playlist:
            first_line = playlist.readline().strip().lstrip('/')
        target = rom.parent / first_line
        if target.exists():
            return target
        _logger.error('%s names %s, which does not exist', rom, target)
        return rom

    @staticmethod
    def _raise_max_map_count(minimum: int = 1048576) -> None:
        # Xenia maps guest memory and its write watches in many small ranges,
        # and demanding titles exhaust the kernel's default of 65530 mappings,
        # after which memory protection fails mid-game. The launcher runs as
        # root, and the setting only lasts until reboot. SteamOS ships a
        # higher value still.
        path = Path('/proc/sys/vm/max_map_count')
        try:
            if int(path.read_text()) < minimum:
                path.write_text(str(minimum))
                _logger.debug('Raised vm.max_map_count to %d', minimum)
        except (OSError, ValueError) as e:
            _logger.warning('Could not raise vm.max_map_count: %s', e)

    def generate(self, system, rom, playersControllers, metadata, guns, wheels, gameResolution):
        for directory in (XENIA_EDGE_STORAGE, XENIA_EDGE_CACHE, XENIA_EDGE_CONTENT, XENIA_EDGE_LOGS):
            mkdir_if_not_exists(directory)
        self._raise_max_map_count()

        config = system.config
        # One log per game, so playing another doesn't replace the last session
        # of the one that had a problem.
        log_name = 'config' if configure_emulator(rom) else rom.stem
        log_file = XENIA_EDGE_LOGS / f'{log_name}.log'
        # Settings go on the command line, overriding xenia-edge.config.toml
        # for this run without rewriting what the user tuned in it.
        command: list[str | Path] = [
            XENIA_EDGE_BIN,
            f'--storage_root={XENIA_EDGE_STORAGE}',
            f'--content_root={XENIA_EDGE_CONTENT}',
            f'--cache_root={XENIA_EDGE_CACHE}',
            f'--log_file={log_file}',
            '--gpu=vulkan',
            '--hid=sdl',
            '--apu=sdl',
            '--mount_cache=true',
            '--mount_scratch=true',
            '--discord=false',
            f'--fullscreen={_flag(config.get_bool("xenia_fullscreen", True))}',
            f'--apply_patches={_flag(config.get_bool("xenia_patches", True))}',
            f'--license_mask={config.get_int("xenia_license", 1)}',
            f'--user_language={config.get_int("xenia_language", 1)}',
            f'--user_country={config.get_int("xenia_country", 103)}',
        ]
        # Batocera's Wine Xenia settings that mean the same here. Not
        # xenia_readback_resolve: for the Wine builds it's D3D12's optional,
        # costly readback, often turned off per game for speed, while here
        # readback_resolve is how GPU output reaches the CPU at all - off, a
        # game that reads it back, such as Fable II for its texture morphs,
        # gets nothing.
        if 'xenia_vsync_fps' in config:
            command.append(f'--framerate_limit={config.get_int("xenia_vsync_fps", 0)}')

        if not configure_emulator(rom):
            command.append(self._resolve_playlist(rom))

        return Command.Command(
            array=command,
            env={
                'SDL_GAMECONTROLLERCONFIG': generate_sdl_game_controller_config(playersControllers),
                # Batocera's session is X11, and Xenia's window is GTK.
                'GDK_BACKEND': 'x11',
            },
        )
