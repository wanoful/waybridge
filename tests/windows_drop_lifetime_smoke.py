#!/usr/bin/env python3
"""Production OLE drop callbacks cancel safely when their target closes."""
import os
from pathlib import Path
import subprocess
from smoke import ROOT


def main():
    build = ROOT / 'build/windows'
    if not (build / 'CMakeCache.txt').exists():
        subprocess.run(['cmake', '-S', 'client', '-B', str(build), '-G', 'Ninja',
                        '-DCMAKE_TOOLCHAIN_FILE=mingw.cmake', '-DCMAKE_BUILD_TYPE=Release'], cwd=ROOT, check=True)
    subprocess.run(['cmake', '--build', str(build), '--target', 'waybridge-drop-lifetime-probe'], cwd=ROOT, check=True)
    output = build / 'waybridge-drop-lifetime-probe.exe'
    (ROOT / 'build/tests').mkdir(parents=True, exist_ok=True)
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
