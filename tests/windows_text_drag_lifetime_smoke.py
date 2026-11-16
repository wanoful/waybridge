#!/usr/bin/env python3
"""Standalone native HGLOBAL/COM reentry adapter test in a disposable Wine prefix."""
import os
from pathlib import Path
import subprocess
import tempfile
from smoke import ROOT


def main():
    target=ROOT/'build/tests/windows-text-drag-lifetime-probe.exe';target.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run(['x86_64-w64-mingw32-g++','-std=c++17','-Wall','-Wextra','-Werror','-static','-O2','-Iclient',
        'tests/windows_text_drag_lifetime_probe.cpp','-o',str(target),'-lole32','-luuid','-lcrypt32'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-text-lease-') as directory:
        env={**os.environ,'WINEPREFIX':str(Path(directory)/'wine'),'WINEDEBUG':'-all',
             'WINEDLLOVERRIDES':'winemenubuilder.exe=d;mscoree,mshtml='}
        try:
            result=subprocess.run(['xvfb-run','-a','wine',str(target)],env=env,capture_output=True,text=True,timeout=90)
            assert result.returncode==0,(result.stdout,result.stderr)
            assert 'PASS:' in result.stdout,result.stdout
            print(result.stdout.strip())
        finally:
            subprocess.run(['wineserver','-k'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10)


if __name__=='__main__':main()
