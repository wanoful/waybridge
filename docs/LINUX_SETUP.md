# Linux Host Setup Guide

This guide walks through setting up a Linux machine to host standalone Waybridge sessions and accept connections from the Windows Control Center.

> **Key prerequisite:** Waybridge shares your Linux user's `HOME`, application profiles, and wallets by default and allows one graphical session per user at a time. **Log out of your local Linux desktop (KDE, GNOME, Hyprland, etc.) before starting a remote Waybridge session**—locking the screen is not enough. See [Session Policy](SESSION_POLICY.md) for details.

---

## 1. Install Dependencies

### Required (Build & Core Runtime)

Waybridge requires:
- **Rust toolchain** (stable, `cargo` + `rustc`, edition 2021)
- **System build libraries**: `pkg-config`, C compiler (`gcc` or `clang`), EGL / GLES, `libdrm`, `wayland`, `libxkbcommon`, and FFmpeg development libraries (`libavcodec`, `libavutil`)
- **Core Linux runtime**: `systemd-logind`, a user D-Bus bus (`dbus-broker` or `dbus-daemon`), Python 3.11+, and GLib/GIO (`libgio-2.0.so.0`)

### Optional (Recommended for Full Desktop Features)

| Feature | Required Packages / Components |
|---|---|
| Hardware video encoding (NVENC) | NVIDIA proprietary driver + FFmpeg built with `h264_nvenc` / `hevc_nvenc` / `av1_nvenc` |
| CUDA GPU color input / experimental HDR | NVIDIA CUDA development headers (`cuda.h`, `cudaGL.h`) at `/opt/cuda` or `WAYBRIDGE_CUDA_ROOT` |
| X11 apps (Java/HMCL, Steam/Proton, X11 Chrome) | `xorg-xwayland` / `xwayland` |
| Session audio & microphone forwarding | PipeWire-Pulse or PulseAudio with `pactl` and `parec` (`libpulse` / `pulseaudio-utils`) |
| Cross-machine read-only file receive | `fuse3` (`fusermount3`) and user access to `/dev/fuse` |
| SVG application icons | `librsvg` (`rsvg-convert`) |
| GTK file picker Portal | `xdg-desktop-portal` + `xdg-desktop-portal-gtk` |
| Gamepads (experimental) | Read/write access to `/dev/uinput` |

### Distribution Package Commands

**Arch Linux:**

```bash
sudo pacman -S --needed \
  base-devel pkgconf rustup \
  mesa libdrm wayland libxkbcommon ffmpeg glib2 python \
  xorg-xwayland pipewire-pulse libpulse fuse3 librsvg \
  xdg-desktop-portal xdg-desktop-portal-gtk
# Optional (NVIDIA CUDA GL interop / experimental HDR):
# sudo pacman -S --needed cuda
```

**Fedora:**

```bash
sudo dnf install \
  gcc gcc-c++ pkgconf-pkg-config \
  mesa-libEGL-devel mesa-libGLES-devel libdrm-devel wayland-devel libxkbcommon-devel \
  ffmpeg-free-devel glib2 python3 \
  xorg-x11-server-Xwayland pipewire-pulseaudio pulseaudio-utils fuse3 librsvg2-tools \
  xdg-desktop-portal xdg-desktop-portal-gtk
# Note: For NVENC support on Fedora, use RPM Fusion's ffmpeg-devel / ffmpeg packages.
```

**Ubuntu / Debian:**

```bash
sudo apt update && sudo apt install -y \
  build-essential pkg-config \
  libegl-dev libgles2-dev libdrm-dev libwayland-dev libxkbcommon-dev \
  libavcodec-dev libavutil-dev libglib2.0-0 python3 \
  xwayland pipewire-pulse pulseaudio-utils fuse3 librsvg2-bin \
  xdg-desktop-portal xdg-desktop-portal-gtk
```

*(Install Rust via [rustup.rs](https://rustup.rs/) if your distro package is older than Rust 1.80.)*

---

## 2. Build the Linux Binaries

From the repository root:

```bash
cargo build --release --workspace --locked
```

This builds:
- `target/release/waybridge` — the headless Wayland compositor and session host
- `target/release/waybridge-service` — the authenticated TLS user service and interactive TUI

If your CUDA headers are installed outside `/opt/cuda` (for example `/usr/local/cuda`), pass `WAYBRIDGE_CUDA_ROOT` during build:

```bash
WAYBRIDGE_CUDA_ROOT=/usr/local/cuda cargo build --release --workspace --locked
```

Verify that FFmpeg exposes NVENC encoders on your host (optional; PNG fallback works without NVENC):

```bash
ffmpeg -hide_banner -encoders | grep nvenc
```

---

## 3. Install User Binaries and systemd Service

Run the installer script from the repository root:

```bash
python3 service/install-user-service.py
```

By default, this installs:
- `~/.local/bin/waybridge` and `~/.local/bin/waybridge-service`
- `~/.config/systemd/user/waybridge.service` (the TLS entry point)
- `~/.config/systemd/user/waybridge-session*.target` (session lifecycle targets)
- `waybridge-portals.conf` and the Waybridge Settings backend descriptor under `~/.local/share/xdg-desktop-portal/` (or `$XDG_DATA_HOME/xdg-desktop-portal/`)

> The installer refuses to overwrite modified user-owned unit files so upgrades never silently clobber your customizations.

### Optional Installer Flags

- **Install or update the Waybridge Portal preset in the user config directory** (use this if a previous installation placed it there):
  ```bash
  python3 service/install-user-service.py \
    --portal-directory "${XDG_CONFIG_HOME:-$HOME/.config}/xdg-desktop-portal"
  ```
- **Install the opt-in notification provider unit** (only if you do not run another notification daemon in your Waybridge session):
  ```bash
  python3 service/install-user-service.py --notification-provider
  ```

### Enable and Start `waybridge.service`

```bash
systemctl --user daemon-reload
systemctl --user enable --now waybridge.service
systemctl --user status waybridge.service
```

### Keep the Service Running When Logged Out (`linger`)

Because you must log out of your local Linux desktop before starting a Waybridge session, enable systemd lingering so your user manager and `waybridge.service` remain running after logout (and start automatically on boot):

```bash
sudo loginctl enable-linger "$USER"
```

Verify lingering is active:

```bash
loginctl show-user "$USER" -p Linger
# Expected: Linger=yes
```

Ensure `~/.local/bin` is on your `PATH` (optional, for convenience when running `waybridge-service` directly).

---

## 4. Configure Waybridge (`config.toml`)

Create `~/.config/waybridge/config.toml` (optional—built-in defaults work out of the box):

```toml
[service]
# Bind to all interfaces, or restrict to a specific LAN/Tailscale/VPN IP:
listen = "0.0.0.0:49092"
compositor = "/home/YOUR_USER/.local/bin/waybridge"
python = "python3"

[session]
fps = 60
bitrate = 30000          # kbps per video stream
codec = "auto"           # auto | png | h264-nvenc | hevc-nvenc | av1-nvenc
gpu_input = "cpu"        # cpu | cuda (NVIDIA GL -> CUDA -> NVENC)
hdr = "off"              # off | experimental-scrgb
layout = "us"            # XKB keyboard layout

[limits]
max_fps = 120
max_bitrate = 200000
allowed_codecs = ["png", "h264", "hevc", "av1"]
```

Validate the configuration before restarting:

```bash
~/.local/bin/waybridge --check-config
~/.local/bin/waybridge --print-config
~/.local/bin/waybridge-service --check-config
```

After editing `[service]`, restart the entry service (existing live sessions are not interrupted):

```bash
systemctl --user restart waybridge.service
```

You can also edit `config.toml`, `[media_budget]`, desktop overrides, and desktop component settings interactively using the built-in terminal UI:

```bash
~/.local/bin/waybridge-service tui
```

See [Configuration and Application Launching](CONFIGURATION.md) and [Media Budgets](MEDIA_BUDGET.md) for all available options.

---

## 5. Networking and Firewall

Waybridge uses **TLS on TCP port `49092`** by default for pairing, session management, control, media, audio, and file channels.

- **LAN / VPN (e.g., Tailscale, WireGuard):** Allow inbound TCP `49092` on your firewall (ideally scoped to your LAN or VPN interface rather than the public internet):
  ```bash
  # UFW example (allow from LAN subnet):
  sudo ufw allow from 192.168.1.0/24 to any port 49092 proto tcp

  # firewalld example:
  sudo firewall-cmd --add-port=49092/tcp --permanent && sudo firewall-cmd --reload
  ```
- **Optional UDP media channel:** If you enable UDP media on a port such as `49091/udp`, allow that UDP port on the same LAN/VPN interface.
- **SSH-only networks:** If you prefer not to expose port `49092`, bind `listen = "127.0.0.1:49092"` in `config.toml` and forward the port from Windows using system SSH:
  ```powershell
  ssh -N -L 127.0.0.1:49092:127.0.0.1:49092 <user>@<linux-host>
  ```
  Then point the Windows Control Center at `127.0.0.1:49092` while still verifying the full TLS certificate fingerprint.

---

## 6. Optional Desktop & Peripheral Setup

### A. XDG Desktop Portal File Pickers and Appearance

The Waybridge Portal preset enables GTK file pickers and publishes the connected client's light or dark preference to supported Linux apps. With XDG Desktop Portal 1.22 or later, the user-installed backend descriptor is discovered automatically. On older releases that support `portals.conf`, install it under `/usr/share/xdg-desktop-portal/portals/`:

```bash
sudo install -Dm644 integration/xdg-desktop-portal/portals/waybridge-appearance.portal \
  /usr/share/xdg-desktop-portal/portals/waybridge-appearance.portal
```

Bind the Portal services to Waybridge's session target so they start with the Waybridge desktop environment and stop on session exit:

```bash
mkdir -p \
  ~/.config/systemd/user/waybridge-session.target.d \
  ~/.config/systemd/user/xdg-desktop-portal.service.d \
  ~/.config/systemd/user/xdg-desktop-portal-gtk.service.d

cat > ~/.config/systemd/user/waybridge-session.target.d/portals.conf <<'EOF'
[Unit]
Wants=xdg-desktop-portal.service xdg-desktop-portal-gtk.service
EOF

cat > ~/.config/systemd/user/xdg-desktop-portal-gtk.service.d/waybridge.conf <<'EOF'
[Unit]
PartOf=waybridge-session.target
After=waybridge-session-pre.target
Before=waybridge-session.target
EOF

cat > ~/.config/systemd/user/xdg-desktop-portal.service.d/waybridge.conf <<'EOF'
[Unit]
PartOf=waybridge-session.target
After=waybridge-session-pre.target xdg-desktop-portal-gtk.service
Before=waybridge-session.target
EOF

systemctl --user daemon-reload
```

See [Desktop Components and Session Lifecycle](INTEGRATION.md) for custom session units and details.

### B. Remote Desktop Notifications

If you installed `waybridge-notifications.service` (Step 3) and no competing daemon owns `org.freedesktop.Notifications` inside Waybridge:

```bash
systemctl --user daemon-reload
systemctl --user enable waybridge-notifications.service
```

### C. Chrome / KWallet Password Backend Override

Because Waybridge runs with its own desktop identity rather than spoofing KDE or GNOME, Chromium may choose a different password backend than your local desktop. If your existing Chrome profile uses KWallet 6, add a Waybridge-only desktop override without touching your system `.desktop` files:

```bash
mkdir -p ~/.config/waybridge/desktop-overrides
cat > ~/.config/waybridge/desktop-overrides/google-chrome.desktop.toml <<'EOF'
version = 1

[default]
launch_mode = "exec"
extra_argv = ["--password-store=kwallet6"]
EOF
```

### D. Gamepad Forwarding (`/dev/uinput`)

Experimental gamepad forwarding requires read/write access to `/dev/uinput` for your user. If `/dev/uinput` is not already accessible to your user (check with `ls -l /dev/uinput`):

```bash
# Ensure the uinput kernel module loads on boot
echo uinput | sudo tee /etc/modules-load.d/uinput.conf
sudo modprobe uinput

# Grant access to the input group (or a dedicated uinput group)
echo 'KERNEL=="uinput", MODE="0660", GROUP="input", OPTIONS+="static_node=uinput"' \
  | sudo tee /etc/udev/rules.d/60-waybridge-uinput.rules
sudo udevadm control --reload-rules
sudo udevadm trigger /dev/uinput
sudo usermod -aG input "$USER"
```

Log out and back in (or reboot) for new group membership to take effect.

---

## 7. Run Pre-Flight Checks and Pair a Windows Client

1. **Check session readiness** (confirm no local Wayland/X11 desktop is running for your UID):
   ```bash
   ~/.local/bin/waybridge --check-session
   ```
   You can also inspect all prerequisites interactively in **`~/.local/bin/waybridge-service tui`** (Tab `1: Overview & Pairing`).

2. **Generate a one-time pairing code** (over local TTY or SSH):
   ```bash
   ~/.local/bin/waybridge-service pair --expires 120
   ```
   This prints:
   - The one-time pairing code
   - The server's full 64-character `certificate_sha256` fingerprint
   - The listen address and port

3. **Pair from Windows Control Center:**
   - Extract `Waybridge-Control-win-{x64,arm64}.zip` and launch `Waybridge.Control.exe`.
   - Click **Add workstation**, enter the Linux host IP/hostname, port `49092`, the verified full `certificate_sha256` fingerprint, and the one-time pairing code.
   - Connect and launch any installed Linux application from the app wall or search bar (`Ctrl+K`).

4. **Manage trusted clients at any time:**
   ```bash
   ~/.local/bin/waybridge-service status
   ~/.local/bin/waybridge-service clients
   ~/.local/bin/waybridge-service revoke <CLIENT_ID>
   ```

---

## 8. Upgrading and Troubleshooting

### Upgrading

1. End any running Waybridge session cleanly (from the Windows Control Center or `waybridge-service tui`).
2. Pull updates and rebuild:
   ```bash
   cargo build --release --workspace --locked
   python3 service/install-user-service.py
   systemctl --user daemon-reload
   systemctl --user restart waybridge.service
   ```
3. Update the Windows Control Center / `waybridge-client.exe` to match.

### Common Issues

- **"Active local graphical session" error when connecting**
  Log out of your local Linux desktop (do not just lock the screen). Run `~/.local/bin/waybridge --check-session` to see which `logind` session is blocking creation.
- **Cannot connect after logging out of the local desktop**
  Make sure lingering is enabled (`sudo loginctl enable-linger "$USER"`) and `waybridge.service` is enabled (`systemctl --user enable --now waybridge.service`).
- **Session logs and service logs**
  ```bash
  journalctl --user -u waybridge.service -n 100 --no-pager
  ls -la "$XDG_RUNTIME_DIR/waybridge-control-center/"
  ```
- **Video falls back to PNG instead of NVENC**
  Confirm the NVIDIA driver is loaded (`nvidia-smi`), FFmpeg supports NVENC (`ffmpeg -encoders | grep nvenc`), and `allowed_codecs` in `config.toml` includes `h264`/`hevc`/`av1`.
- **Cross-machine file receive fails on Linux**
  Check that `fusermount3` is installed (`which fusermount3`) and `/dev/fuse` is readable/writable by your user.
- **Notifications report "another daemon owns the bus"**
  Check which process holds the bus name with `busctl --user status org.freedesktop.Notifications` and disable competing notification daemons inside your Waybridge session if you want Waybridge to handle notifications.
- **Direct low-level testing without `waybridge-service`**
  See [Usage & Build Guide](README.md#running-directly-low-level-cli) for running `./target/release/waybridge -- weston-terminal` over an SSH tunnel.
