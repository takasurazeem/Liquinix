#!/usr/bin/env bash
# Smoke test for the Linux programs, without a screen: starts a headless KWin (kwin_wayland --virtual), runs
# liquinix-dock inside it and checks, in the Wayland protocol log, that the Dock got a layer-shell surface anchored
# to the bottom edge, acknowledged its size and committed a drawn frame. Used by the CI (.github/workflows/build.yml)
# and runnable by hand:
#
#   tools/linux/ci-smoke.sh [./build/liquinix-dock]
#
# Exits 0 when everything was seen, 1 otherwise (and prints what was missing and the end of the logs).
set -uo pipefail

DOCK="${1:-./build/liquinix-dock}"
SOCKET="wayland-ci-$$"
[[ -x "$DOCK" ]] || { echo "not found or not executable: $DOCK" >&2; exit 1; }
command -v kwin_wayland >/dev/null || { echo "kwin_wayland not found (install kwin)" >&2; exit 1; }

# KWin wants a D-Bus session: run the whole script inside a private one
if [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]] && command -v dbus-run-session >/dev/null && [[ -z "${CI_SMOKE_IN_DBUS:-}" ]]; then
    CI_SMOKE_IN_DBUS=1 exec dbus-run-session -- "$0" "$@"
fi

# KWin needs a private runtime dir (sockets) and a D-Bus session; nothing here touches a real desktop
export XDG_RUNTIME_DIR="$(mktemp -d)"; chmod 700 "$XDG_RUNTIME_DIR"
LOGS="$(mktemp -d)"
cleanup() { [[ -n "${KWIN:-}" ]] && kill "$KWIN" 2>/dev/null; wait 2>/dev/null; rm -rf "$XDG_RUNTIME_DIR" "$LOGS"; }
trap cleanup EXIT
unset WAYLAND_DISPLAY DISPLAY

# Fedora ships kwin_wayland with a file capability (cap_sys_nice, for realtime scheduling). Containers can't grant
# it, so the kernel refuses to run the binary ("Operation not permitted"). A plain copy has no capability attached;
# a headless test doesn't need realtime priority.
KWIN_BIN="$(command -v kwin_wayland)"
if ! "$KWIN_BIN" --version >/dev/null 2>&1; then
    cp "$KWIN_BIN" "$XDG_RUNTIME_DIR/kwin_wayland" && KWIN_BIN="$XDG_RUNTIME_DIR/kwin_wayland"
fi

"$KWIN_BIN" --virtual --socket "$SOCKET" --width 1280 --height 800 --no-lockscreen --no-global-shortcuts \
    >"$LOGS/kwin.log" 2>&1 &
KWIN=$!
for _ in $(seq 100); do [[ -S "$XDG_RUNTIME_DIR/$SOCKET" ]] && break; sleep 0.1; done
if [[ ! -S "$XDG_RUNTIME_DIR/$SOCKET" ]]; then
    echo "KWin didn't start:"; tail -30 "$LOGS/kwin.log"; exit 1
fi

WAYLAND_DISPLAY="$SOCKET" QT_QPA_PLATFORM=wayland WAYLAND_DEBUG=client LIQUINIX_SAFE_MODE=1 \
    timeout 10 "$DOCK" >"$LOGS/dock.log" 2>&1
rc=$?
[[ $rc -eq 124 ]] || { echo "liquinix-dock exited early (code $rc):"; tail -30 "$LOGS/dock.log"; exit 1; }

log="$LOGS/dock.log"; fail=0
check() { if grep -Eq "$2" "$log"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
check "started"                         'liquinix-dock: '
check "layer-shell surface (top layer)" 'zwlr_layer_shell_v1#[0-9]+\.get_layer_surface\(.*, 2, "liquinix-dock"\)'
# anchors are bits (top 1, bottom 2, left 4, right 8): any combination with the bottom edge
check "anchored to the bottom edge"     'zwlr_layer_surface_v1#[0-9]+\.set_anchor\((2|3|6|7|10|11|14|15)\)'
check "configured by KWin"              'zwlr_layer_surface_v1#[0-9]+\.ack_configure\('
# a frame after the configure: a buffer attached and committed on a surface
if awk '/zwlr_layer_surface_v1#[0-9]+\.ack_configure\(/{f=1} f && /wl_surface#[0-9]+\.attach\(wl_buffer/{a=1} a && /wl_surface#[0-9]+\.commit\(\)/{ok=1; exit} END{exit !ok}' "$log"; then
    echo "ok   drew and committed a frame"
else
    echo "FAIL drew and committed a frame"; fail=1
fi
if [[ $fail -ne 0 ]]; then
    echo "--- liquinix-dock (last lines)"; grep -v '^\[' "$log" | tail -20
    echo "--- kwin"; tail -20 "$LOGS/kwin.log"
fi
exit $fail
