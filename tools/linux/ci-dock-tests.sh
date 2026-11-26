#!/usr/bin/env bash
# Behaviour tests for liquinix-dock without a screen: scripted mouse events (LIQUINIX_DOCK_SCRIPT) on the offscreen Qt
# platform, with fake apps whose Exec is `true` (a click launches nothing real). Used by the CI and runnable by hand:
#
#   tools/linux/ci-dock-tests.sh [./build/liquinix-dock]
#
# Checks: dragging a pinned app reorders the Dock and saves dock.txt; dragging it out unpins it; the right-click menu
# unpins and opens, and runs the app's own actions and Open at Login; a click makes the icon bounce, and the bounce stops after 8 s; automatic hiding comes and goes;
# settings changes are applied live.
# Exits 0 when all pass.
set -uo pipefail

DOCK="${1:-./build/liquinix-dock}"
[[ -x "$DOCK" ]] || { echo "not found or not executable: $DOCK" >&2; exit 1; }
DIR="$(mktemp -d)"
trap '[[ -n "${KEEP:-}" ]] || rm -rf "$DIR"' EXIT; [[ -n "${KEEP:-}" ]] && echo "keeping $DIR"
export XDG_DATA_HOME="$DIR/data" XDG_CONFIG_HOME="$DIR/config" QT_QPA_PLATFORM=offscreen
export LANG=C.UTF-8 LC_ALL=C.UTF-8  # the Dock follows the system language when idioma= isn't set: the tests use English
mkdir -p "$XDG_DATA_HOME/applications" "$XDG_CONFIG_HOME/liquinix"
# the fake apps' icon: an XPM written here (Qt reads it without plugins or an icon theme, which CI doesn't have)
{ echo '/* XPM */'; echo 'static char *icon[] = { "16 16 2 1", "  c None", "x c #2F8CF0",'
  for _ in $(seq 16); do echo '"xxxxxxxxxxxxxxxx",'; done; echo '};'; } > "$DIR/icon.xpm"
for a in a b c d; do
    printf '[Desktop Entry]\nType=Application\nName=Fake %s\nIcon=%s\nExec=true\n' "$a" "$DIR/icon.xpm" > "$XDG_DATA_HOME/applications/fake-$a.desktop"
done
# fake-a has its own actions, like Firefox's "New Private Window"
printf 'Actions=new-window;broken;\n\n[Desktop Action new-window]\nName=New Fake Window\nExec=true --new-window %%u\n\n[Desktop Action broken]\nName=No Exec\n' >> "$XDG_DATA_HOME/applications/fake-a.desktop"
printf 'tamanho=48\nampliacao=0\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"
reset() { printf 'fake-a\nfake-b\nfake-c\nfake-d\n' > "$XDG_CONFIG_HOME/liquinix/dock.txt"; }
saved() { grep -v '^#' "$XDG_CONFIG_HOME/liquinix/dock.txt" | paste -sd, -; }
# every run's full output also goes to all.log: sanitizer reports (a sanitized build in CI) fail the tests at the end
run() { LIQUINIX_DOCK_SCRIPT="$1" timeout 60 "$DOCK" 2>&1 | tee -a "$DIR/all.log" | sed -n 's/^liquinix-dock: pinned: //p' | tr -d ' '; }
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
reset; launched=$(LIQUINIX_DOCK_SCRIPT="rclick 501 140; menu Open" timeout 60 "$DOCK" 2>&1 | tee -a "$DIR/all.log" | grep -c "launch fake-a (true): ok")
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
       LIQUINIX_DOCK_SCRIPT="wait 2000" timeout 60 "$DOCK" 2>&1 | tee -a "$DIR/all.log"; wait )
check "settings applied live: new icon size" "$(grep -c 'settings reloaded: icon 64' <<<"$out")" "1"
check "settings applied live: new pinned list" "$(sed -n 's/^liquinix-dock: pinned: //p' <<<"$out" | tr -d ' ')" "fake-d,fake-a"
printf 'tamanho=48\nampliacao=0\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"

# the Dock on the left edge (dock_posicao=esquerda): the same drag, turned a quarter. The window is 420 x 800, the apps'
# centres are at y = 261, 312, 363, 414 along the edge, with their icons about 36 points from the left edge
printf 'tamanho=48\nampliacao=0\ndock_posicao=esquerda\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"
reset; got=$(run "move 36 261; press; move 36 280; move 36 320; move 36 340; release")
check "left edge: drag the first app between the third and the fourth" "$got" "fake-b,fake-c,fake-a,fake-d"
printf 'tamanho=48\nampliacao=0\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"

# translations: with idioma=fr the right-click menu is in French (the Windows tables)
printf 'tamanho=48\nampliacao=0\nidioma=fr\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"
reset; got=$(run "rclick 552 140; menu Supprimer du Dock")
check "idioma=fr: the menu is in French (Supprimer du Dock)" "$got" "fake-a,fake-c,fake-d"
printf 'tamanho=48\nampliacao=0\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"

# Downloads as a stack: newest first, unfinished downloads left out, a file picked by name
mkdir -p "$DIR/downloads"; printf 'XDG_DOWNLOAD_DIR="%s"\n' "$DIR/downloads" > "$XDG_CONFIG_HOME/user-dirs.dirs"
i=0; for f in old.pdf photo.png notes.txt partial.iso.part; do i=$((i+1)); echo x > "$DIR/downloads/$f"; touch -d "2026-11-0$i 10:00" "$DIR/downloads/$f"; done
reset; out=$(LIQUINIX_DOCK_SCRIPT="move 728 140; press; release; stack notes.txt" timeout 60 "$DOCK" 2>&1 | tee -a "$DIR/all.log")
check "Downloads opens as a stack, newest first" "$(sed -n 's/^liquinix-dock: stack: //p' <<<"$out")" "notes.txt, photo.png, old.pdf"
check "a file in the stack can be picked" "$(grep -c 'stack "notes.txt": chosen' <<<"$out")" "1"

# badges: an app's count sent over D-Bus (the Unity LauncherEntry API) shows on its icon, and goes when the app hides it
reset; printf 'tamanho=48\nampliacao=0\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"
cat > "$DIR/badge.sh" <<BADGE
#!/usr/bin/env bash
emit() { gdbus emit --session --object-path /test/fake_b --signal com.canonical.Unity.LauncherEntry.Update "'application://fake-b.desktop'" "\$1" >/dev/null; }
LIQUINIX_DOCK_SCRIPT="wait 300; png $DIR/nobadge.png; wait 1200; png $DIR/badge.png; wait 1000; png $DIR/badge-off.png" timeout 60 "$DOCK" &
sleep 0.9; emit "{'count': <int64 7>, 'count-visible': <true>}"
sleep 1.0; emit "{'count-visible': <false>}"
wait
BADGE
chmod +x "$DIR/badge.sh"
if command -v gdbus >/dev/null && command -v dbus-run-session >/dev/null; then
    out=$(dbus-run-session -- "$DIR/badge.sh" 2>&1 | tee -a "$DIR/all.log")
    check "a badge arrives over D-Bus" "$(grep -c 'badge fake-b: 7$' <<<"$out")" "1"
    check "the app hides its badge" "$(grep -c 'badge fake-b: 7 (hidden)' <<<"$out")" "1"
    if [[ -s "$DIR/badge.png" ]] && ! cmp -s "$DIR/nobadge.png" "$DIR/badge.png"; then echo "ok   the badge is drawn"; else echo "FAIL the badge is drawn"; fail=1; fi
    if cmp -s "$DIR/nobadge.png" "$DIR/badge-off.png"; then echo "ok   a hidden badge isn't drawn"; else echo "FAIL a hidden badge isn't drawn"; fail=1; fi
else
    echo "FAIL badges: gdbus and dbus-run-session are needed (glib2, dbus-daemon)"; fail=1
fi

# the app menu: the app's own actions (.desktop), and Open at Login (an autostart copy that comes and goes)
reset; out=$(LIQUINIX_DOCK_SCRIPT="rclick 501 140; menupng $DIR/appmenu.png 0; menu New Fake Window" timeout 60 "$DOCK" 2>&1 | tee -a "$DIR/all.log")
check "the menu runs the app's own action" "$(grep -c 'action fake-a "New Fake Window" (true --new-window): ok' <<<"$out")" "1"
check "an action without Exec isn't listed" "$(LIQUINIX_DOCK_SCRIPT="rclick 501 140; menu No Exec" timeout 60 "$DOCK" 2>&1 | tee -a "$DIR/all.log" | grep -c 'menu "No Exec": not found')" "1"
reset; LIQUINIX_DOCK_SCRIPT="rclick 501 140; menu Open at Login" timeout 60 "$DOCK" >>"$DIR/all.log" 2>&1
check "Open at Login adds an autostart entry" "$(grep -c '^Exec=true$' "$XDG_CONFIG_HOME/autostart/fake-a.desktop" 2>/dev/null)" "1"
LIQUINIX_DOCK_SCRIPT="rclick 501 140; menu Open at Login" timeout 60 "$DOCK" >>"$DIR/all.log" 2>&1
check "and turning it off removes it" "$([[ -e "$XDG_CONFIG_HOME/autostart/fake-a.desktop" ]] && echo present || echo gone)" "gone"

# dock_downloads=0 takes Downloads out of the Dock (the Windows key)
printf 'tamanho=48\nampliacao=0\ndock_downloads=0\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"
reset; check "dock_downloads=0: no Downloads" "$(LIQUINIX_DOCK_SCRIPT="click Downloads" timeout 60 "$DOCK" 2>&1 | tee -a "$DIR/all.log" | grep -c 'click "Downloads": not found')" "1"
printf 'tamanho=48\nampliacao=0\n' > "$XDG_CONFIG_HOME/liquinix/ajustes.txt"
reset; check "Downloads is there by default" "$(LIQUINIX_DOCK_SCRIPT="click Downloads" timeout 60 "$DOCK" 2>&1 | tee -a "$DIR/all.log" | grep -c 'click "Downloads": done')" "1"

# a sanitized build reports memory errors, leaks and undefined behaviour in the output: any of those fails the run
if grep -qE "ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:" "$DIR/all.log"; then
    echo "FAIL sanitizer reports:"; grep -E -A12 "ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:" "$DIR/all.log" | head -60; fail=1
else
    echo "ok   no sanitizer reports"
fi
exit $fail
