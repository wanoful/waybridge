#!/usr/bin/env python3
"""Regression for missing default PRI: matching resources and actual MRT lookups."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    project = ET.parse(ROOT/"gui/Waybridge.Control/Waybridge.Control.csproj").getroot()
    version = project.find(".//WindowsAppSdkVersion").text
    cache = Path(os.environ.get("NUGET_PACKAGES", Path.home()/".nuget/packages"))
    for arch in ("arm64", "x64"):
        output = ROOT/"build"/f"control-center-{arch}"
        framework = cache/f"microsoft.windowsappsdk.runtime/{version}/tools/MSIX/win10-{arch}/Microsoft.WindowsAppRuntime.1.8.msix"
        with zipfile.ZipFile(framework) as package:
            assert (output/"resources.pri").read_bytes() == package.read("resources.pri"), "default application PRI missing or changed"
            for name in ("Microsoft.ui.xaml.dll", "Microsoft.UI.Xaml.Controls.pri", "Microsoft.WindowsAppRuntime.dll", "MRM.dll", "Microsoft.WindowsAppRuntime.Insights.Resource.dll"):
                entry = next(n for n in package.namelist() if n.lower() == name.lower())
                local = next(p for p in output.iterdir() if p.name.lower() == name.lower())
                assert local.read_bytes() == package.read(entry), f"{arch} resource/runtime version mismatch: {name}"
        print(f"PASS: {arch} default resource index and exact matching WinUI/AppRuntime/MRT/notification-registration binaries")
    if "--wine" not in sys.argv:
        return
    probe = ROOT/"build/tests/windows-resources-probe.exe"
    subprocess.run(["x86_64-w64-mingw32-g++", "-municode", "-static", "-O2", "tests/windows_resources_probe.cpp", "-o", str(probe)], cwd=ROOT, check=True)
    env = {**os.environ, "WINEPREFIX": str(ROOT/"build/wine-gui-ui"), "WINEDEBUG": "-all"}
    library = "Z:" + str(ROOT/"build/control-center-x64/MRM.dll")
    def resolve(file, success):
        result = subprocess.run(["wine", str(probe), library, "Z:" + str(file)], env=env, capture_output=True, text=True, timeout=30)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        if success:
            assert result.stdout.count("PASS: embedded XBF theme") == 2
    for arch in ("arm64", "x64"):
        resolve(ROOT/"build"/f"control-center-{arch}"/"resources.pri", True)
    with tempfile.TemporaryDirectory(prefix="waybridge-missing-pri-") as temp:
        resolve(Path(temp)/"resources.pri", False)
    resolve(ROOT/"build/control-center-x64/Microsoft.Windows.Workloads.pri", False)
    print("PASS: real MRT Core resolves both theme XBFs from both indexes; missing/unrelated PRI reproduces lookup failure")


if __name__ == "__main__":
    main()
