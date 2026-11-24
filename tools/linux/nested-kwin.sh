#!/usr/bin/env bash
# Start a nested KWin in a window on the current Wayland desktop, for testing Liquinix's Linux apps without
# touching the real session. If something crashes or hangs in there, only this window goes away.
#
#   tools/linux/nested-kwin.sh                         # empty nested session, 1600x900
#   tools/linux/nested-kwin.sh ./build/liquinix-dock   # start a program inside it
#   WIDTH=1280 HEIGHT=800 SCALE=2 tools/linux/nested-kwin.sh
#
# Then, from another terminal, run programs inside it with:
#   WAYLAND_DISPLAY=wayland-liquinix ./build/liquinix-dock
#
# Close the window (or press Ctrl+C here) to end it. Requires a KDE Plasma Wayland session (kwin_wayland 6).
set -euo pipefail

SOCKET="${SOCKET:-wayland-liquinix}"
WIDTH="${WIDTH:-1600}"
HEIGHT="${HEIGHT:-900}"
SCALE="${SCALE:-1}"

if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "WAYLAND_DISPLAY is not set: run this from a Wayland desktop session." >&2
    exit 1
fi
if [[ "$WAYLAND_DISPLAY" == "$SOCKET" ]]; then
    echo "Already inside the nested session ($SOCKET)." >&2
    exit 1
fi
if [[ -e "${XDG_RUNTIME_DIR:?}/$SOCKET" ]]; then
    echo "$XDG_RUNTIME_DIR/$SOCKET already exists: a nested KWin is probably running (or a stale socket was left)." >&2
    echo "Use another name with SOCKET=wayland-liquinix2, or remove the stale socket." >&2
    exit 1
fi
command -v kwin_wayland >/dev/null || { echo "kwin_wayland not found (install kwin)." >&2; exit 1; }

echo "Nested KWin on WAYLAND_DISPLAY=$SOCKET (${WIDTH}x${HEIGHT}, scale $SCALE)."
echo "Run programs in it with: WAYLAND_DISPLAY=$SOCKET <program>"

# --wayland-display makes KWin a window on the current desktop. No lock screen and no global shortcuts, so the
# nested session never grabs keys like Meta or locks itself. --xwayland lets X11-only tools run inside too.
exec kwin_wayland \
    --wayland-display "$WAYLAND_DISPLAY" \
    --socket "$SOCKET" \
    --width "$WIDTH" --height "$HEIGHT" --scale "$SCALE" \
    --no-lockscreen --no-global-shortcuts \
    --xwayland \
    "$@"
