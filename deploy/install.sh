#!/usr/bin/env bash
# LogiNext — one-shot installer for Arch / CachyOS.
#
# Builds the C++ daemon and the Tauri UI in release mode, installs both into
# ~/.local/bin, registers a desktop entry + icon so the app is searchable from
# the launcher, and installs (but does not enable) the systemd user unit.
#
# Idempotent: re-running upgrades an existing install in place.

set -euo pipefail

# ---- locate repo root (script lives in deploy/) -----------------------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"
cd "$REPO_ROOT"

# ---- destinations -----------------------------------------------------------
BIN_DIR="$HOME/.local/bin"
APPS_DIR="$HOME/.local/share/applications"
ICONS_DIR="$HOME/.local/share/icons"
SYSTEMD_USER_DIR="$HOME/.config/systemd/user"

DAEMON_DST="$BIN_DIR/loginext"
UI_DST="$BIN_DIR/loginext-ui"
DESKTOP_DST="$APPS_DIR/loginext.desktop"
ICON_DST="$ICONS_DIR/loginext.png"

# ---- pretty print -----------------------------------------------------------
c_blue='\033[1;34m'; c_green='\033[1;32m'; c_yellow='\033[1;33m'; c_red='\033[1;31m'; c_off='\033[0m'
step()  { printf "${c_blue}==>${c_off} %s\n" "$*"; }
ok()    { printf "${c_green}  ✓${c_off} %s\n" "$*"; }
warn()  { printf "${c_yellow}  !${c_off} %s\n" "$*"; }
die()   { printf "${c_red}  ✗ %s${c_off}\n" "$*" >&2; exit 1; }

# ---- enable_service flag ----------------------------------------------------
ENABLE_SERVICE=0
for arg in "$@"; do
    case "$arg" in
        --enable-service) ENABLE_SERVICE=1 ;;
        -h|--help)
            cat <<EOF
Usage: deploy/install.sh [--enable-service]

  --enable-service   After install, enable + start loginext.service as a
                     systemd --user unit. Off by default — the UI spawns the
                     daemon on demand and that is the recommended workflow.
EOF
            exit 0
            ;;
        *) die "unknown argument: $arg" ;;
    esac
done

[[ $EUID -eq 0 ]] && die "Do not run as root. The script will sudo for pacman only."

# ---- 1. system deps ---------------------------------------------------------
step "Installing system dependencies (pacman)"
PAC_PKGS=(base-devel cmake ninja pkgconf libevdev nodejs npm rustup webkit2gtk-4.1 gtk3 libayatana-appindicator librsvg)
if command -v pacman &>/dev/null; then
    sudo pacman -S --needed --noconfirm "${PAC_PKGS[@]}"
    ok "pacman packages present"
else
    warn "pacman not found — skipping system-dep step (non-Arch host?)"
fi

if command -v rustup &>/dev/null && ! rustup show active-toolchain &>/dev/null; then
    step "Installing default Rust toolchain (rustup)"
    rustup default stable
fi

# ---- 2. build daemon --------------------------------------------------------
step "Building C++ daemon (Release)"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build --config Release
[[ -x build/loginext ]] || die "daemon build did not produce build/loginext"
ok "daemon built"

# ---- 3. build UI ------------------------------------------------------------
step "Building Tauri UI (release)"
pushd ui >/dev/null
[[ -d node_modules ]] || npm install
npm run tauri build
popd >/dev/null

# Tauri puts the binary at ui/src-tauri/target/release/<bin name from Cargo.toml>.
UI_SRC="ui/src-tauri/target/release/loginext-ui"
[[ -x "$UI_SRC" ]] || die "Tauri build did not produce $UI_SRC"
ok "UI built"

# ---- 4. install binaries ----------------------------------------------------
step "Installing binaries to $BIN_DIR"
mkdir -p "$BIN_DIR"
install -m 0755 build/loginext "$DAEMON_DST"
install -m 0755 "$UI_SRC"      "$UI_DST"
ok "$DAEMON_DST"
ok "$UI_DST"

case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) warn "$BIN_DIR is not on \$PATH — add it to your shell rc to run 'loginext' from a terminal." ;;
esac

# ---- 5. icon ----------------------------------------------------------------
step "Installing icon"
mkdir -p "$ICONS_DIR"
ICON_SRC="ui/src-tauri/icons/icon.png"
[[ -f "$ICON_SRC" ]] || die "icon not found at $ICON_SRC"
install -m 0644 "$ICON_SRC" "$ICON_DST"
ok "$ICON_DST"

# ---- 6. desktop entry -------------------------------------------------------
step "Installing desktop entry"
mkdir -p "$APPS_DIR"
cat > "$DESKTOP_DST" <<EOF
[Desktop Entry]
Type=Application
Name=LogiNext
GenericName=Logitech Device Control
Comment=Configure Logitech mouse bindings and sensitivity
Exec=$UI_DST
Icon=$ICON_DST
Terminal=false
Categories=Utility;Settings;HardwareSettings;
Keywords=logitech;mouse;mx;master;input;
StartupWMClass=LogiNext
EOF
chmod 0644 "$DESKTOP_DST"
ok "$DESKTOP_DST"

if command -v update-desktop-database &>/dev/null; then
    update-desktop-database "$APPS_DIR" &>/dev/null || true
fi
if command -v gtk-update-icon-cache &>/dev/null; then
    gtk-update-icon-cache -q "$ICONS_DIR" &>/dev/null || true
fi

# ---- 7. systemd user unit ---------------------------------------------------
step "Installing systemd user unit"
mkdir -p "$SYSTEMD_USER_DIR"
UNIT_SRC="deploy/systemd/loginext.service"
UNIT_DST="$SYSTEMD_USER_DIR/loginext.service"
# Rewrite ExecStart so it points at the user-local install instead of /usr/local/bin.
sed "s|^ExecStart=.*|ExecStart=$DAEMON_DST --quiet|" "$UNIT_SRC" > "$UNIT_DST"
chmod 0644 "$UNIT_DST"
ok "$UNIT_DST"

systemctl --user daemon-reload || warn "systemctl --user daemon-reload failed (no user manager?)"

if [[ $ENABLE_SERVICE -eq 1 ]]; then
    step "Enabling loginext.service"
    systemctl --user enable --now loginext.service
    ok "service enabled and started"
else
    warn "service installed but NOT enabled. The UI spawns the daemon on demand;"
    warn "pass --enable-service if you want it to start at every login."
fi

# ---- done -------------------------------------------------------------------
printf "\n${c_green}LogiNext installed.${c_off}\n"
echo "  • Launch from your application menu (search 'LogiNext'),"
echo "  • or run: $UI_DST"
echo "  • Daemon log: \$XDG_STATE_HOME/loginext/daemon.log (default ~/.local/state/loginext/daemon.log)"
