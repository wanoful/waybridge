#!/usr/bin/env python3
"""Production OLE drop callbacks cancel safely when their target closes."""
import os
from pathlib import Path
import subprocess
from smoke import ROOT


def main():
    output = ROOT / 'build/tests/windows-drop-lifetime.exe'
    output.parent.mkdir(parents=True, exist_ok=True)
    libraries = ('secur32 bcrypt ws2_32 gdiplus gdi32 ole32 crypt32 shell32 uuid d3d11 dxgi dcomp '
                 'd3dcompiler mfuuid wmcodecdspuuid strmiids oleaut32 windowscodecs imm32')
    subprocess.run(['x86_64-w64-mingw32-g++', '-std=c++17', '-Wall', '-Wextra', '-Wpedantic',
                    '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
                    '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-municode', '-static', '-O1',
                    'tests/windows_drop_lifetime_probe.cpp', '-o', str(output),
                    *['-l' + library for library in libraries.split()]], cwd=ROOT, check=True)
    prefix = ROOT / 'build/wine-drop-lifetime'
    prefix.mkdir(mode=0o700, parents=True, exist_ok=True)
    env = {**os.environ, 'WINEPREFIX': str(prefix), 'WINEDEBUG': '-all',
           'WINEDLLOVERRIDES': 'winemenubuilder.exe=d;mscoree,mshtml='}
    read, write = os.pipe()
    with (ROOT / 'build/tests/windows-drop-lifetime.log').open('w+') as log:
        xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write), '-screen', '0', '1024x768x24',
                                 '-nolisten', 'tcp'], pass_fds=[write], stdout=log, stderr=log)
        os.close(write)
        with os.fdopen(read) as pipe:
            env['DISPLAY'] = ':' + pipe.readline().strip()
        try:
            subprocess.run(['wineboot', '-u'], env=env, stdout=log, stderr=log, timeout=60, check=True)
            result = subprocess.run(['wine', str(output)], env=env, capture_output=True, text=True, timeout=30)
            log.write(result.stdout + result.stderr)
            log.flush()
            assert result.returncode == 0, Path(log.name).read_text()
            print(result.stdout.strip())
        finally:
            subprocess.run(['wineserver', '-k'], env=env, stdout=log, stderr=log, timeout=10)
            xvfb.terminate()
            xvfb.wait(timeout=10)


if __name__ == '__main__':
    main()
