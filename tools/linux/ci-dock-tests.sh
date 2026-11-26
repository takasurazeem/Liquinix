#!/usr/bin/env bash
# Behaviour tests for liquinix-dock without a screen: scripted mouse events (LIQUINIX_DOCK_SCRIPT) on the offscreen Qt
# platform, with fake apps whose Exec is `true` (a click launches nothing real). Used by the CI and runnable by hand:
#
#   tools/linux/ci-dock-tests.sh [./build/liquinix-dock]
#
# Checks: dragging a pinned app reorders the Dock and saves dock.txt; dragging it out unpins it; the right-click menu
# unpins and opens; a click makes the icon bounce, and the bounce stops after 8 s; automatic hiding comes and goes;
# settings changes are applied live.
# Exits 0 when all pass.
set -uo pipefail

DOCK="${1:-./build/liquinix-dock}"
[[ -x "$DOCK" ]] || { echo "not found or not executable: $DOCK" >&2; exit 1; }
DIR="$(mktemp -d)"
trap '[[ -n "${KEEP:-}" ]] || rm -rf "$DIR"' EXIT; [[ -n "${KEEP:-}" ]] && echo "keeping $DIR"
export XDG_DATA_HOME="$DIR/data" XDG_CONFIG_HOME="$DIR/config" QT_QPA_PLATFORM=offscreen
mkdir -p "$XDG_DATA_HOME/applications" "$XDG_CONFIG_HOME/liquinix"
# the fake apps' icon: an XPM written here (Qt reads it without plugins or an icon theme, which CI doesn't have)
{ echo '/* XPM */'; echo 'static char *icon[] = { "16 16 2 1", "  c None", "x c #2F8CF0",'
  for _ in $(seq 16); do echo '"xxxxxxxxxxxxxxxx",'; done; echo '};'; } > "$DIR/icon.xpm"
for a in a b c d; do
    printf '[Desktop Entry]\nType=Application\nName=Fake %s\nIcon=%s\nExec=true\n' "$a" "$DIR/icon.xpm" > "$XDG_DATA_HOME/applications/fake-$a.desktop"
done
printf 'tamanho=48\nampliacao=0\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"
reset() { printf 'fake-a\nfake-b\nfake-c\nfake-d\n' > "$XDG_CONFIG_HOME/liquinix/dock.txt"; }
saved() { grep -v '^#' "$XDG_CONFIG_HOME/liquinix/dock.txt" | paste -sd, -; }
run() { LIQUINIX_DOCK_SCRIPT="$1" timeout 60 "$DOCK" 2>&1 | sed -n 's/^liquinix-dock: pinned: //p' | tr -d ' '; }
fail=0
check() { if [[ "$2" == "$3" ]]; then echo "ok   $1"; else echo "FAIL $1: got '$2', expected '$3'"; fail=1; fi; }

# 1280 points wide, magnification off: the four apps' centres are at x = 501, 552, 603, 654, icons around y = 140
reset; got=$(run "move 501 140; press; move 520 140; move 560 140; move 580 140; release")
check "drag the first app between the third and the fourth" "$got" "fake-b,fake-c,fake-a,fake-d"
check "the new order is saved to dock.txt" "$(saved)" "fake-b,fake-c,fake-a,fake-d"
reset; got=$(run "move 654 140; press; move 630 140; move 560 140; move 480 140; release")
check "drag the last app to the front" "$got" "fake-d,fake-a,fake-b,fake-c"
reset; got=$(run "move 501 140; press; move 501 40; move 501 -80; release")
check "drag the first app out of the Dock to unpin it" "$got" "fake-b,fake-c,fake-d"
check "the unpinned app is gone from dock.txt" "$(saved)" "fake-b,fake-c,fake-d"

# the right-click menu: Remove from Dock unpins; Open launches
reset; got=$(run "rclick 552 140; menu Remove from Dock")
check "right-click > Remove from Dock unpins the app" "$got" "fake-a,fake-c,fake-d"
reset; launched=$(LIQUINIX_DOCK_SCRIPT="rclick 501 140; menu Open" timeout 60 "$DOCK" 2>&1 | grep -c "launch fake-a (true): ok")
check "right-click > Open launches the app" "$launched" "1"

# a click bounces the icon (Exec=true never opens a window, so it bounces for the full 8 s), then it stops. The resting
# picture is taken once the hover has settled (its label fades in), so only the bounce can make the pictures differ.
reset; run "move 552 140; wait 400; png $DIR/rest.png; press; release; wait 200; png $DIR/hop.png; wait 8200; png $DIR/after.png" >/dev/null
if [[ -s "$DIR/rest.png" && -s "$DIR/hop.png" ]] && ! cmp -s "$DIR/rest.png" "$DIR/hop.png"; then echo "ok   a click makes the icon bounce"; else echo "FAIL a click makes the icon bounce"; fail=1; fi
if cmp -s "$DIR/rest.png" "$DIR/after.png"; then echo "ok   the bounce stops after 8 s"; else echo "FAIL the bounce stops after 8 s"; fail=1; fi
# automatic hiding (ocultar=1): hidden, shown once the pointer touches the bottom edge, hidden again when it leaves
printf 'tamanho=48\nampliacao=0\nocultar=1\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"
reset; run "wait 300; png $DIR/hidden.png; move 640 199; wait 600; png $DIR/shown.png; leave; wait 600; png $DIR/hidden2.png" >/dev/null
if [[ -s "$DIR/shown.png" ]] && ! cmp -s "$DIR/hidden.png" "$DIR/shown.png"; then echo "ok   autohide: the Dock comes back at the bottom edge"; else echo "FAIL autohide: the Dock comes back at the bottom edge"; fail=1; fi
if cmp -s "$DIR/hidden.png" "$DIR/hidden2.png"; then echo "ok   autohide: it hides again when the pointer leaves"; else echo "FAIL autohide: it hides again when the pointer leaves"; fail=1; fi
printf 'tamanho=48\nampliacao=0\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"

# settings applied live: ajustes.txt (saved by renaming a new file over it, as editors do) and dock.txt change while the
# Dock runs, and it picks up the new icon size and pinned list without a restart
reset; out=$( ( sleep 0.8; printf 'tamanho=64\nampliacao=0\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.new" \
              && mv "$XDG_CONFIG_HOME/liquinix/ajustes.new" "$XDG_CONFIG_HOME/liquinix/ajustes.txt"
              printf 'fake-d\nfake-a\n' > "$XDG_CONFIG_HOME/liquinix/dock.txt" ) &
       LIQUINIX_DOCK_SCRIPT="wait 2000" timeout 60 "$DOCK" 2>&1; wait )
check "settings applied live: new icon size" "$(grep -c 'settings reloaded: icon 64' <<<"$out")" "1"
check "settings applied live: new pinned list" "$(sed -n 's/^liquinix-dock: pinned: //p' <<<"$out" | tr -d ' ')" "fake-d,fake-a"
printf 'tamanho=48\nampliacao=0\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"

exit $fail
