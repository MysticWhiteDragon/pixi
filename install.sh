#!/bin/sh
# Pixi installer for Raspberry Pi OS Lite (Trixie, 64-bit), for the PSPi and similar.
#
# Run on the Pi from this folder:
#
#     sudo sh install.sh                 # install, boot straight into Pixi
#     sudo sh install.sh --no-autostart  # install only; start it yourself with: pixi-session
#     sudo sh install.sh --no-steamlink  # skip Steam Link
#
# What it does:
#   1. Installs what Lite leaves out: wlroots, Mesa (KMS/GLES) drivers, SDL2, XWayland,
#      Japanese fonts, NetworkManager, PipeWire audio, RetroArch with emulator cores,
#      Steam Link and the foot terminal.
#   2. Builds Pixi here on the Pi and installs it under /usr/local.
#   3. Puts a starter config in ~/.config/pixi/pixi.conf and creates ~/roms/<system>.
#   4. Gives the user access to the screen, GPU, controls, audio, network and backlight.
#   5. Unless --no-autostart: logs that user in on tty1 and starts Pixi there.
#      Logging out of Pixi (Guide -> power -> Log out) leaves you at a shell; type
#      pixi-session to go back.
#
# Safe to run again: it updates Pixi and replaces its own settings instead of duplicating them.

set -eu

say()  { printf '\n\033[1;35m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

autostart=true
steamlink=true
for arg in "$@"; do
    case "$arg" in
        --no-autostart) autostart=false ;;
        --no-steamlink) steamlink=false ;;
        -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
        *) die "unknown option: $arg (try --help)" ;;
    esac
done

[ "$(id -u)" -eq 0 ] || exec sudo sh "$0" "$@"
src_dir="$(cd "$(dirname "$0")" && pwd)"
[ -f "$src_dir/compositor/pixi.c" ] || die "run this from the pixi folder (compositor/pixi.c not found)"

# ---------------------------------------------------------------- checks
. /etc/os-release
case "${VERSION_CODENAME:-}" in
    trixie|forky|sid) ;;
    bookworm) die "Pixi needs Raspberry Pi OS based on Debian 13 (Trixie). This is Bookworm, whose wlroots is too old. Flash a current Raspberry Pi OS Lite image." ;;
    *) warn "untested release '${VERSION_CODENAME:-unknown}'; continuing" ;;
esac
[ "$(dpkg --print-architecture)" = arm64 ] || warn "this isn't 64-bit Raspberry Pi OS; continuing anyway"

target_user="${SUDO_USER:-}"
if [ -z "$target_user" ] || [ "$target_user" = root ]; then
    target_user="$(getent passwd 1000 | cut -d: -f1)"
fi
[ -n "$target_user" ] || die "couldn't work out which user to set up; run with sudo from your normal account"
target_home="$(getent passwd "$target_user" | cut -d: -f6)"
target_group="$(id -gn "$target_user")"
say "Setting up Pixi for '$target_user' on ${PRETTY_NAME:-Linux}"

config_txt=/boot/firmware/config.txt
[ -f "$config_txt" ] || config_txt=/boot/config.txt
if [ -f "$config_txt" ] && ! grep -Eq '^[[:space:]]*dtoverlay=vc4-(f)?kms-v3d' "$config_txt"; then
    warn "$config_txt has no dtoverlay=vc4-kms-v3d line. Pixi needs the KMS graphics driver;"
    warn "if the screen stays black, add 'dtoverlay=vc4-kms-v3d' under [all] and reboot."
fi

# ---------------------------------------------------------------- packages
export DEBIAN_FRONTEND=noninteractive
say "Updating package lists"
apt-get update

say "Installing build tools and libraries"
apt-get install -y --no-install-recommends \
    build-essential pkg-config \
    libwlroots-0.18-dev libwayland-dev wayland-protocols libxkbcommon-dev \
    libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libsdl2-mixer-dev libgles-dev

say "Installing graphics, fonts, audio, networking and session services"
apt-get install -y --no-install-recommends \
    libgl1-mesa-dri libegl1 libegl-mesa0 libgles2 libgbm1 \
    xwayland fonts-noto-cjk hicolor-icon-theme \
    dbus dbus-user-session \
    network-manager \
    pipewire pipewire-pulse pipewire-alsa wireplumber alsa-utils \
    foot

say "Installing RetroArch"
apt-get install -y retroarch

say "Installing emulator cores (any this release doesn't package are skipped)"
for core in libretro-nestopia libretro-snes9x libretro-gambatte libretro-mgba \
            libretro-genesisplusgx libretro-beetle-pce-fast libretro-beetle-psx \
            libretro-desmume libretro-bsnes-mercury-performance; do
    if apt-get install -y "$core" >/dev/null 2>&1; then echo "  installed $core"
    else echo "  not available: $core"; fi
done

if [ "$steamlink" = true ]; then
    say "Installing Steam Link"
    if ! apt-get install -y steamlink; then
        warn "Steam Link isn't available from this system's package sources."
        warn "It comes from the Raspberry Pi OS repository (archive.raspberrypi.com)."
    fi
fi

# ---------------------------------------------------------------- build Pixi
say "Building Pixi"
build_dir="$(mktemp -d)"
cp -r "$src_dir/compositor" "$src_dir/shell" "$src_dir/protocol" "$src_dir/assets" "$src_dir/session" \
      "$src_dir/Makefile" "$src_dir/pixi.conf" "$build_dir/"
make -C "$build_dir" clean >/dev/null 2>&1 || true
make -C "$build_dir" PREFIX=/usr/local >/dev/null
make -C "$build_dir" install PREFIX=/usr/local >/dev/null
rm -rf "$build_dir"
echo "  installed $(/usr/local/bin/pixi --version) and $(/usr/local/bin/pixi-shell --version)"

# ---------------------------------------------------------------- user config + rom folders
say "Creating ~/.config/pixi and ~/roms"
as_user() { runuser -u "$target_user" -- "$@"; }
as_user mkdir -p "$target_home/.config/pixi"
if [ ! -f "$target_home/.config/pixi/pixi.conf" ]; then
    install -o "$target_user" -g "$target_group" -m 0644 "$src_dir/pixi.conf" "$target_home/.config/pixi/pixi.conf"
    echo "  wrote ~/.config/pixi/pixi.conf"
else
    echo "  kept your existing ~/.config/pixi/pixi.conf"
fi
for sys in nes snes gb gbc gba md sms gg pce psx psp nds n64; do
    as_user mkdir -p "$target_home/roms/$sys"
done

# ---------------------------------------------------------------- permissions
say "Giving $target_user access to the screen, GPU, controls, audio and network"
for group in video render input audio netdev; do
    getent group "$group" >/dev/null && usermod -aG "$group" "$target_user"
done
cat > /etc/udev/rules.d/90-pixi-backlight.rules <<'EOF'
SUBSYSTEM=="backlight", ACTION=="add", RUN+="/bin/chgrp video /sys%p/brightness", RUN+="/bin/chmod g+w /sys%p/brightness"
EOF
udevadm control --reload-rules 2>/dev/null || true
udevadm trigger --subsystem-match=backlight 2>/dev/null || true
systemctl enable --now NetworkManager >/dev/null 2>&1 || warn "couldn't start NetworkManager"

# ---------------------------------------------------------------- autostart
profile="$target_home/.profile"
autologin_dir=/etc/systemd/system/getty@tty1.service.d
# Clear this installer's block, and the ones the earlier pibar and LineXinBar
# installers wrote, so only one thing starts on tty1.
if [ -f "$profile" ]; then
    for name in pixi pibar LineXinBar; do
        sed -i "/^# >>> $name autostart >>>\$/,/^# <<< $name autostart <<<\$/d" "$profile"
    done
fi
rm -f "$autologin_dir/pixi-autologin.conf" "$autologin_dir/pibar-autologin.conf" "$autologin_dir/linexinbar-autologin.conf"

if [ "$autostart" = true ]; then
    say "Logging $target_user in automatically on tty1 and starting Pixi there"
    mkdir -p "$autologin_dir"
    cat > "$autologin_dir/pixi-autologin.conf" <<EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin $target_user --noclear %I \$TERM
EOF
    systemctl set-default multi-user.target >/dev/null
    cat >> "$profile" <<'EOF'
# >>> pixi autostart >>>
# Start Pixi on the first console. Remove this block (or rerun install.sh with
# --no-autostart) to stop it. Touch ~/.no-pixi to skip it for one login.
if [ "$(tty)" = /dev/tty1 ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    if [ -e "$HOME/.no-pixi" ]; then
        rm -f "$HOME/.no-pixi"
    else
        pixi-session
        echo
        echo "Pixi has exited. Type 'pixi-session' to start it again."
    fi
fi
# <<< pixi autostart <<<
EOF
    chown "$target_user:$target_group" "$profile"
    systemctl daemon-reload 2>/dev/null || true
fi

# ---------------------------------------------------------------- done
say "Done"
if [ "$autostart" = true ]; then
    echo "Reboot to start Pixi:  sudo reboot"
else
    echo "Log out and back in (for the new groups), then run pixi-session from the console."
fi
cat <<'EOF'

  * Guide: Select + Start together (or a controller's Guide button, or Super/Home on a keyboard).
  * Games: copy game files into ~/roms/<system> (nes, snes, gba, md, psx ...).
  * Steam Link: keep your PC on with Steam open, start Steam Link from Games,
    and type the PIN it shows into Steam on the PC.
EOF
