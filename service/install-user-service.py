#!/usr/bin/env python3
"""Install a built entry point and a user unit; activation remains explicit."""
import argparse
import os
from pathlib import Path
import shutil


def unit_quote(path):
    return str(path).replace("%", "%%").replace("\\", "\\\\").replace('"', '\\"')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--service-binary", type=Path, default=Path("target/release/waybridge-service"))
    parser.add_argument("--compositor-binary", type=Path, default=Path("target/release/waybridge"))
    parser.add_argument("--prefix", type=Path, default=Path.home() / ".local")
    parser.add_argument("--unit-directory", type=Path, default=Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config"))) / "systemd/user")
    args = parser.parse_args()
    service_source = args.service_binary.resolve(strict=True)
    compositor_source = args.compositor_binary.resolve(strict=True)
    binary_directory = args.prefix.expanduser().resolve() / "bin"
    binary_directory.mkdir(parents=True, exist_ok=True)
    for source, name in [(service_source, "waybridge-service"), (compositor_source, "waybridge")]:
        destination = binary_directory / name
        if source != destination:
            shutil.copy2(source, destination)
        destination.chmod(0o755)
    template = (Path(__file__).parent / "waybridge.service.in").read_text()
    template = template.replace("@SERVICE_BINARY@", unit_quote(binary_directory / "waybridge-service"))
    template = template.replace("@COMPOSITOR_BINARY@", unit_quote(binary_directory / "waybridge"))
    args.unit_directory.mkdir(parents=True, exist_ok=True)
    (args.unit_directory / "waybridge.service").write_text(template)
    print("Installed user binaries and waybridge.service.")
    print("Start explicitly: systemctl --user daemon-reload && systemctl --user enable --now waybridge.service")
    print(f"Create a pairing code: {binary_directory / 'waybridge-service'} pair")


if __name__ == "__main__":
    main()
