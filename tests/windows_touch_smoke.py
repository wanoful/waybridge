#!/usr/bin/env python3
"""Production native touch adapter under a private Wine/Xvfb desktop."""
import os
from pathlib import Path
import subprocess
import tempfile
from smoke import ROOT


def main():
    target = ROOT/'build/tests/windows-touch-probe.exe'
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['x86_64-w64-mingw32-g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-static', '-O2', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-Iclient',
                    'tests/windows_touch_probe.cpp', '-o', str(target)], cwd=ROOT, check=True)
    window_target = ROOT/'build/tests/windows-touch-window-probe.exe'
    subprocess.run(['x86_64-w64-mingw32-g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-static', '-O2', '-Iclient', 'tests/windows_touch_window_probe.cpp',
                    '-o', str(window_target)], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-touch-') as directory:
        env = {**os.environ, 'WINEPREFIX': str(Path(directory)/'wine'), 'WINEDEBUG': '-all',
               'WINEDLLOVERRIDES': 'winemenubuilder.exe=d;mscoree,mshtml='}
        try:
            result = subprocess.run(['xvfb-run', '-a', 'wine', str(target)], env=env,
                                    capture_output=True, text=True, timeout=90)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert 'PASS:' in result.stdout, result.stdout
            print(result.stdout.strip())
            window_result = subprocess.run(['xvfb-run', '-a', 'wine', str(window_target)], env=env,
                                           capture_output=True, text=True, timeout=30)
            assert window_result.returncode == 0, (window_result.stdout, window_result.stderr)
            print(window_result.stdout.strip())
        finally:
            subprocess.run(['wineserver', '-k'], env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=10)


if __name__ == '__main__':
    main()
