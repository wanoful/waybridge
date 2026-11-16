#!/usr/bin/env python3
"""Actual user32/Shell tray and nested popup menus, in a disposable Wine desktop."""
import os
from pathlib import Path
import subprocess
import tempfile
from smoke import ROOT

def main():
    build=ROOT/'build/tests';build.mkdir(parents=True,exist_ok=True)
    subprocess.run(['x86_64-w64-mingw32-g++','-std=c++17','-O1','-static','-DUNICODE','-D_UNICODE','-DWINVER=0x0A00','-D_WIN32_WINNT=0x0A00','-Iclient','tests/windows_tray_probe.cpp','-o',str(build/'windows-tray-probe.exe'),'-lole32','-lwindowscodecs','-luuid','-lgdi32','-lshell32','-lcrypt32'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-tray-wine-') as temporary:
        log=(build/'windows-tray-wine.log').open('w');read,write=os.pipe()
        x=subprocess.Popen(['Xvfb','-displayfd',str(write),'-screen','0','1280x800x24','-nolisten','tcp'],pass_fds=[write],stdout=log,stderr=log);os.close(write)
        with os.fdopen(read) as pipe:display=':'+pipe.readline().strip()
        env={**os.environ,'DISPLAY':display,'WINEPREFIX':str(Path(temporary)/'prefix'),'WINEDEBUG':'-all','WINEDLLOVERRIDES':'winemenubuilder.exe=d;mscoree,mshtml='}
        try:
            result=subprocess.run(['wine',str(build/'windows-tray-probe.exe')],env=env,capture_output=True,text=True,timeout=90)
            print(result.stdout);assert result.returncode==0,result.stderr
            assert 'menu_checked=1 menu_clicked=1' in result.stdout
            print('PASS: production Win32 tray callbacks, native nested/check/radio/disabled HMENU, real keyboard mnemonic action, input disable, Explorer re-add and cleanup')
            # shell_rect may be 0 in Wine; native Windows Explorer acceptance remains separate.
        finally:
            subprocess.run(['wineserver','-k'],env=env,stdout=log,stderr=log);x.terminate();x.wait(timeout=5);log.close()
if __name__=='__main__':main()
