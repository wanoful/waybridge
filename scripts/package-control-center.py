#!/usr/bin/env python3
"""Check PE architectures and self-contained dependencies, then create a ZIP."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile

root = Path(__file__).resolve().parents[1]
arch = sys.argv[1]
assert arch in ("arm64", "x64")
directory = root / "build" / ("control-center-" + arch)
expected = 0xaa64 if arch == "arm64" else 0x8664
for name in ("Waybridge.Control.exe", "waybridge.exe", "waybridge-client.exe", "coreclr.dll", "Microsoft.UI.Xaml.dll", "Microsoft.WindowsAppRuntime.dll", "Microsoft.WindowsAppRuntime.Insights.Resource.dll"):
    path = next(p for p in directory.iterdir() if p.name.lower() == name.lower())
    data = path.read_bytes()
    offset, = struct.unpack_from("<I", data, 0x3c)
    assert data[offset:offset+4] == b"PE\0\0", name
    machine, = struct.unpack_from("<H", data, offset+4)
    assert machine == expected, (name, hex(machine), arch)
config = json.loads((directory / "Waybridge.Control.runtimeconfig.json").read_text())
manifest_exe = (directory / "Waybridge.Control.exe").read_bytes()
assert b'Microsoft.UI.Xaml.Application' in manifest_exe and b'PerMonitorV2' in manifest_exe, "missing embedded WinUI/DPI manifest"
start = manifest_exe.index(b"<assembly ")
end = manifest_exe.index(b"</assembly>", start) + len(b"</assembly>")
manifest = ET.fromstring(manifest_exe[start:end])
assert list(manifest)[0].tag.endswith("assemblyIdentity"), "assembly identity must lead the manifest"
assert len(manifest.findall(".//{urn:schemas-microsoft-com:winrt.v1}activatableClass")) > 100
assert config["runtimeOptions"]["includedFrameworks"] and "frameworks" not in config["runtimeOptions"]
cli_config = json.loads((directory / "waybridge.runtimeconfig.json").read_text())
assert cli_config["runtimeOptions"]["includedFrameworks"] == config["runtimeOptions"]["includedFrameworks"]
assert (directory / "waybridge.dll").is_file() and (directory / "waybridge.deps.json").is_file()
for name in ("waybridge.ico", "waybridge.png", "waybridge-badge.png"):
    assert (directory / "Assets" / name).is_file(), "missing GUI branding resource: " + name
assert (directory / "Microsoft.UI.Xaml.Controls.pri").is_file()
assert (directory / "resources.pri").read_bytes().startswith(b"mrm_pri"), "missing default merged WinUI resource index"
files = sorted(p for p in directory.rglob("*") if p.is_file() and p.suffix != ".pdb" and p.name != "SHA256SUMS.txt")
(directory / "SHA256SUMS.txt").write_text("".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.relative_to(directory).as_posix() + "\n" for p in files))
archive = root / "build" / f"Waybridge-Control-win-{arch}.zip"
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
    for path in [*files, directory / "SHA256SUMS.txt"]:
        bundle.write(path, "Waybridge/" + path.relative_to(directory).as_posix())
print(f"PASS: {arch} GUI, streaming worker and native WinUI/.NET runtimes; {archive} ({archive.stat().st_size // (1024*1024)} MiB)")
