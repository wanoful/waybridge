#!/usr/bin/env python3
"""Production SDL adapter with SDL virtual devices in a private Wine prefix."""
import os
from pathlib import Path
import subprocess
import tempfile
from smoke import ROOT


def main():
    build = ROOT / 'build/windows'
    if not (build / 'CMakeCache.txt').exists():
        subprocess.run(['cmake', '-S', 'client', '-B', str(build), '-G', 'Ninja',
                        '-DCMAKE_TOOLCHAIN_FILE=mingw.cmake', '-DCMAKE_BUILD_TYPE=Release'], cwd=ROOT, check=True)
    subprocess.run(['cmake', '--build', str(build), '--target', 'waybridge-gamepad-probe'], cwd=ROOT, check=True)
    executable = build / 'waybridge-gamepad-probe.exe'
    with tempfile.TemporaryDirectory(prefix='waybridge-native-gamepad-') as directory:
        env = {**os.environ, 'WINEPREFIX': str(Path(directory) / 'wine'), 'WINEDEBUG': '-all',
               'WINEDLLOVERRIDES': 'winemenubuilder.exe=d;mscoree,mshtml='}
        try:
            result = subprocess.run(['xvfb-run', '-a', 'wine', str(executable)], env=env,
                                    capture_output=True, text=True, timeout=90)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert 'PASS' in result.stdout, result.stdout
            print(result.stdout.strip())
        finally:
            subprocess.run(['wineserver', '-k'], env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=10)


if __name__ == '__main__':
    main()
