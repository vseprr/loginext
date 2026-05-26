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
KWIN_SCRIPT_DIR="$HOME/.local/share/kwin/scripts/loginext-focus"

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
# v1.0.0 default: enable + start loginext.service so the daemon comes up at
# every login and survives reboots without the user having to open the UI
# first. Pass --no-enable to keep the unit installed but inactive — useful
# for headless / debugging setups where you want manual `systemctl --user
# start` control.
ENABLE_SERVICE=1
for arg in "$@"; do
    case "$arg" in
        --enable-service) ENABLE_SERVICE=1 ;;          # accepted for back-compat
        --no-enable)      ENABLE_SERVICE=0 ;;
        -h|--help)
            cat <<EOF
Usage: deploy/install.sh [--no-enable]

  (default)         Build + install the daemon and UI, then enable+start
                    loginext.service as a systemd --user unit. The daemon
                    auto-starts at every login from now on.
                    Also calls 'loginctl enable-linger \$USER' so the daemon
                    runs across boots even on tty-only / headless setups
                    (idempotent; no-op on a GUI host with a display manager).
  --no-enable       Skip the systemd enable+start step. The unit file is
                    still installed; bring it up manually with
                    'systemctl --user enable --now loginext.service'.
EOF
            exit 0
            ;;
        *) die "unknown argument: $arg" ;;
    esac
done

[[ $EUID -eq 0 ]] && die "Do not run as root. The script will sudo for pacman only."

# ---- 1. system deps ---------------------------------------------------------
step "Installing system dependencies (pacman)"
PAC_PKGS=(
    base-devel cmake ninja pkgconf
    libevdev libxcb xcb-util-wm wayland systemd-libs
    nodejs npm rustup
    webkit2gtk-4.1 gtk3 libayatana-appindicator librsvg
)
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
# We only need the raw binary — `--bundles deb` skips AppImage/RPM packaging
# (AppImage wants a square icon, and we install via .desktop instead). If the
# bundler still trips on something cosmetic, tolerate it: the binary is what
# we care about, and we re-check its presence below.
#
# `npm run tauri:build` (composite script in package.json) runs `vite build`
# explicitly before invoking tauri. Tauri's own `beforeBuildCommand` would
# do the same, but routing through the composite makes the Vite step
# visible in build logs and gives `install.sh` a single canonical command
# matching the developer-facing instructions in ui/src-tauri/README.md.
step "Building Tauri UI (release)"
pushd ui >/dev/null
[[ -d node_modules ]] || npm install
npm run tauri:build -- --bundles deb || warn "tauri bundler exited non-zero — checking the raw binary anyway"
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
# WEBKIT_DISABLE_DMABUF_RENDERER=1 mirrors the dev-time wrapper in
# ui/package.json; without it webkit2gtk-4.1 crashes on launch on many
# Wayland sessions (CachyOS included).
Exec=env WEBKIT_DISABLE_DMABUF_RENDERER=1 $UI_DST
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

# ---- 7. KWin focus-bridge script (KDE Plasma 6) ----------------------------
# Plasma 6's KWin no longer advertises org_kde_plasma_window_management to
# regular wayland clients, so the daemon's protocol backend can't see native
# Wayland windows on a stock Plasma session. The KWin script below listens
# for workspace.windowActivated and forwards each event to the daemon's
# `org.loginext.WindowFocus.Activated` D-Bus method (the same pattern
# kdotool uses). Installing it on non-KDE systems is harmless — KWin scripts
# are only loaded by KWin.
step "Installing KWin focus-bridge script"
KWIN_SCRIPT_SRC="$REPO_ROOT/deploy/kwin/loginext-focus"
if [[ -d "$KWIN_SCRIPT_SRC" ]]; then
    mkdir -p "$KWIN_SCRIPT_DIR/contents/code"
    install -m 0644 "$KWIN_SCRIPT_SRC/metadata.json"           "$KWIN_SCRIPT_DIR/metadata.json"
    install -m 0644 "$KWIN_SCRIPT_SRC/contents/code/main.js"   "$KWIN_SCRIPT_DIR/contents/code/main.js"
    ok "$KWIN_SCRIPT_DIR"
else
    warn "deploy/kwin/loginext-focus not found — skipping KWin bridge"
fi

# Enable + reload the script if the user is on KDE. We try Plasma 6 tools
# first (kwriteconfig6 / qdbus6), then Plasma 5 (kwriteconfig5 / qdbus).
# All steps are best-effort: a non-KDE host (or a headless install) just
# skips this block and the daemon falls back to X11/Hyprland/etc.
if [[ -d "$KWIN_SCRIPT_DIR" ]]; then
    KW_OK=0
    for KW_TOOL in kwriteconfig6 kwriteconfig5; do
        if command -v "$KW_TOOL" &>/dev/null; then
            "$KW_TOOL" --file kwinrc --group Plugins \
                --key loginext-focusEnabled true >/dev/null 2>&1 || true
            KW_OK=1
            break
        fi
    done
    if [[ $KW_OK -eq 1 ]]; then
        # Ask KWin to re-read its config. If KWin isn't running (e.g. the
        # install is happening over SSH before login), the qdbus call fails
        # silently and the script will pick up the change on the next login.
        for QD_TOOL in qdbus6 qdbus-qt6 qdbus; do
            if command -v "$QD_TOOL" &>/dev/null; then
                "$QD_TOOL" org.kde.KWin /KWin reconfigure >/dev/null 2>&1 || true
                break
            fi
        done
        ok "KWin focus bridge enabled (loginext-focusEnabled=true)"
    else
        warn "kwriteconfig{5,6} not found — enable the script manually:"
        warn "  System Settings → Window Management → KWin Scripts → LogiNext Focus Bridge"
    fi
fi

# ---- 7.5 udev rules (system-wide) ------------------------------------------
# These let the daemon run unprivileged by granting the active session user
# ACL access to /dev/input/eventN (the MX Master 3S) and /dev/uinput. We
# install to /etc/udev/rules.d/ rather than /usr/lib/udev/rules.d/ because
# install.sh is a per-host operator script (not a package) — /etc is the
# right home for admin-managed overrides, and the path takes precedence
# over /usr/lib so a future PKGBUILD install doesn't accidentally shadow
# an admin's customisation here.
step "Installing udev rules (requires sudo)"
UDEV_SRC="$REPO_ROOT/deploy/udev/99-loginext.rules"
UDEV_DST="/etc/udev/rules.d/99-loginext.rules"
if [[ -f "$UDEV_SRC" ]]; then
    if sudo install -Dm0644 "$UDEV_SRC" "$UDEV_DST"; then
        ok "$UDEV_DST"
        # Reload + trigger so the rules apply immediately, no replug needed.
        # `input` subsystem covers /dev/input/event*; `misc` covers /dev/uinput.
        # Both are best-effort: a CI / chroot install with no live udevd
        # will fail these silently, and the rules apply on next boot anyway.
        sudo udevadm control --reload-rules >/dev/null 2>&1 || \
            warn "udevadm reload-rules failed — replug the receiver to pick up the rule manually."
        sudo udevadm trigger --subsystem-match=input --subsystem-match=misc \
            >/dev/null 2>&1 || true
        ok "udev rules reloaded"
    else
        warn "could not install udev rules (sudo declined?) — daemon will need 'input' group membership or sudo to run."
    fi
else
    warn "$UDEV_SRC not found — skipping udev rules step"
fi

# ---- 8. systemd user unit ---------------------------------------------------
# Note: the UI's service.rs heal logic owns this file after the first
# `loginext-ui` launch. If the canonical template (deploy/systemd/loginext.service)
# diverges from service.rs's `TEMPLATE_VERSION`, the UI rewrites this
# file in place from the in-Rust template. Keep the two in lockstep —
# users on a current loginext-ui will get the latest template either
# way, but a stale source-of-truth here would be confusing during code
# review.
step "Installing systemd user unit"
mkdir -p "$SYSTEMD_USER_DIR"
UNIT_SRC="deploy/systemd/loginext.service"
UNIT_DST="$SYSTEMD_USER_DIR/loginext.service"
# Rewrite ExecStart so it points at the user-local install instead of /usr/local/bin.
sed "s|^ExecStart=.*|ExecStart=$DAEMON_DST --quiet|" "$UNIT_SRC" > "$UNIT_DST"
chmod 0644 "$UNIT_DST"
ok "$UNIT_DST"

systemctl --user daemon-reload || warn "systemctl --user daemon-reload failed (no user manager?)"

# ---- 8.5 enable lingering (best-effort) -------------------------------------
# `loginctl enable-linger $USER` makes the user's systemd manager start at
# boot independent of an active login session. On a GUI host with a display
# manager (SDDM/GDM/lightdm) the user manager already starts when you log
# in, so this is mostly belt-and-braces for tty-only setups and ssh-only
# admins. Idempotent: a second invocation is a no-op.
#
# We do this BEFORE the enable+start step below so the symlink we're about
# to write lands under a manager that's guaranteed to come up at boot.
if command -v loginctl &>/dev/null; then
    if ! loginctl show-user "$USER" 2>/dev/null | grep -q "Linger=yes"; then
        if sudo loginctl enable-linger "$USER" 2>/dev/null; then
            ok "lingering enabled for $USER (daemon survives logout)"
        else
            warn "loginctl enable-linger failed — daemon will only run during active sessions. Re-run install.sh as root if you need headless operation."
        fi
    else
        ok "lingering already enabled for $USER"
    fi
fi

if [[ $ENABLE_SERVICE -eq 1 ]]; then
    step "Enabling loginext.service"
    systemctl --user enable --now loginext.service
    ok "service enabled and started — autostarts at every login"
else
    warn "service installed but NOT enabled (--no-enable was passed). Bring it up"
    warn "manually with: systemctl --user enable --now loginext.service"
fi

# ---- done -------------------------------------------------------------------
printf "\n${c_green}LogiNext installed.${c_off}\n"
echo "  • Launch from your application menu (search 'LogiNext'),"
echo "  • or run: $UI_DST"
echo "  • Daemon log: \$XDG_STATE_HOME/loginext/daemon.log (default ~/.local/state/loginext/daemon.log)"
echo
echo "  No sudo required — the udev rules grant your session user direct"
echo "  access to the mouse and /dev/uinput. If the daemon reports"
echo "  'permission denied' on /dev/input/event*, replug the Bolt receiver"
echo "  once so logind re-applies its uaccess ACL."
