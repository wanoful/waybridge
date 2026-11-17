#!/usr/bin/env python3
"""Production PT_PEN adapter with controlled native samples on a private desktop."""
import os
from pathlib import Path
import subprocess
import tempfile
from smoke import ROOT

def main():
    executable=ROOT/'build/tests/windows-pen-probe.exe';executable.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run(['x86_64-w64-mingw32-g++','-std=c++17','-Wall','-Wextra','-Werror','-static','-O2',
                    '-DWINVER=0x0A00','-D_WIN32_WINNT=0x0A00','-Iclient','tests/windows_pen_probe.cpp','-o',str(executable)],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-native-pen-') as directory:
        env={**os.environ,'WINEPREFIX':str(Path(directory)/'wine'),'WINEDEBUG':'-all','WINEDLLOVERRIDES':'winemenubuilder.exe=d;mscoree,mshtml='}
        try:
            result=subprocess.run(['xvfb-run','-a','wine',str(executable)],env=env,capture_output=True,text=True,timeout=90)
            assert result.returncode==0,(result.stdout,result.stderr)
            assert 'PASS:' in result.stdout,result.stdout
            print(result.stdout.strip())
        finally:subprocess.run(['wineserver','-k'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10)
if __name__=='__main__':main()
