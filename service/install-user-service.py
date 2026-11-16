#!/usr/bin/env python3
"""Install built entrypoints and standard user units; activation remains explicit."""
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
    parser.add_argument("--portal-directory", type=Path,
                        help="explicit standard Portal configuration directory (e.g. /usr/share/xdg-desktop-portal); omitted leaves Portal configuration unchanged")
    parser.add_argument("--notification-provider", action="store_true",
                        help="install the optional Waybridge notification provider unit; does not enable it or replace another provider")
    args = parser.parse_args()
    service_source = args.service_binary.resolve(strict=True)
    compositor_source = args.compositor_binary.resolve(strict=True)
    binary_directory = args.prefix.expanduser().resolve() / "bin"
    template = (Path(__file__).parent / "waybridge.service.in").read_text()
    template = template.replace("@SERVICE_BINARY@", unit_quote(binary_directory / "waybridge-service"))
    template = template.replace("@COMPOSITOR_BINARY@", unit_quote(binary_directory / "waybridge"))
    integration = Path(__file__).resolve().parents[1] / "integration"
    files = {args.unit_directory / "waybridge.service": template.encode()}
    files.update((args.unit_directory / source.name, source.read_bytes())
                 for source in sorted((integration / "systemd/user").glob("*.target")))
    if args.notification_provider:
        notification_template = (integration / "systemd/user/waybridge-notifications.service.in").read_text()
        files[args.unit_directory / "waybridge-notifications.service"] = notification_template.replace(
            "@COMPOSITOR_BINARY@", unit_quote(binary_directory / "waybridge")).encode()
    if args.portal_directory:
        source = integration / "xdg-desktop-portal/waybridge-portals.conf"
        files[args.portal_directory / source.name] = source.read_bytes()
    # Preflight every user-owned file before any writes, including binaries.
    for destination, data in files.items():
        if destination.is_symlink() or (destination.exists() and destination.read_bytes() != data):
            raise SystemExit(f"Refusing to overwrite modified user configuration: {destination}; merge it manually")
    binary_directory.mkdir(parents=True, exist_ok=True)
    for source, name in [(service_source, "waybridge-service"), (compositor_source, "waybridge")]:
        destination = binary_directory / name
        if source != destination:
            shutil.copy2(source, destination)
        destination.chmod(0o755)
    for destination, data in files.items():
        if not destination.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
    print("Installed user binaries, waybridge.service and Waybridge session targets.")
    print("Session targets activate only when the compositor publishes its display environment.")
    print("Start explicitly: systemctl --user daemon-reload && systemctl --user enable --now waybridge.service")
    print(f"Create a pairing code: {binary_directory / 'waybridge-service'} pair")
    if args.notification_provider:
        print("Notification provider installed but not enabled. Select it explicitly, with no other notification server in this session:")
        print("  systemctl --user enable waybridge-notifications.service")


if __name__ == "__main__":
    main()
