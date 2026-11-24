#!/usr/bin/env bash
# Emergency stop: end every running Liquinix process on Linux (dock, menu bar, panels...).
# Bind it to a global shortcut in System Settings > Keyboard > Shortcuts, so you can recover even if a Liquinix
# window covers the screen or grabs input.
#
# It matches process names that start with "liquinix" (the convention for every Linux binary in this project),
# not whole command lines. That way it never kills your editor, terminal or git just because the repository
# folder is named Liquinix.
set -u

if ! pgrep '^liquinix' >/dev/null; then
    echo "No Liquinix processes running."
    exit 0
fi
pgrep -a '^liquinix'
pkill -TERM '^liquinix'
for _ in 1 2 3 4 5 6 7 8 9 10; do
    pgrep '^liquinix' >/dev/null || { echo "Stopped."; exit 0; }
    sleep 0.2
done
echo "Still running after 2 s; killing."
pkill -KILL '^liquinix'
