# Developing Liquinix on Linux

How to build and test Liquinix's Linux side on the same computer you use every day, without risking your own
desktop session. The Linux side is just starting: `liquinix-dock` is a placeholder surface for now (see
`docs/linux-roadmap.md` for the plan and `.claude/skills/liquinix-platform-layer/SKILL.md` for the architecture).

The approach has four layers, from the one you use all day to the one you rarely need:

| Layer | Use it for | If something goes wrong |
|---|---|---|
| [Toolbox container](#1-build-in-a-toolbox) | compilers, `-devel` packages, building | delete the container |
| [Nested KWin](#2-run-in-a-nested-kwin) | running and debugging the dock, menu bar and panels | close the window |
| [Test user account](#3-test-the-real-desktop-in-a-second-account) | the real experience: login, autostart, performance | switch back to your account |
| [Virtual machine](#4-a-virtual-machine-for-root-level-work) | anything run as root, compositor or session changes, packages | delete the VM |

Written for KDE Plasma 6 on Wayland on Fedora-based systems (tested with Plasma and KWin 6.7.5 on Nobara 44).
GNOME notes are at the end.

## Ground rules

- **Never `sudo make install`.** Run programs from the build folder, or install to `~/.local`. Files in `/usr`
  are where a mistake can break login for every user.
- **No autostart in your own account.** Liquinix starts automatically only in the test account, and only once
  it's stable, so a crash at login can never lock you out.
- **Commit and push before testing on a real session.**
- **Name every Linux binary `liquinix-…`** (`liquinix-dock`, `liquinix-menubar`). The kill switch below relies
  on it.

## 1. Build in a toolbox

A [toolbox](https://containertoolbx.org) is a container that shares your home folder, your user and your Wayland
session, so programs built in it run on your desktop normally. The compilers and libraries stay inside the
container, and your system packages stay as they are.

```sh
# Nobara and other Fedora derivatives aren't recognized automatically, so name the image:
toolbox create --distro fedora --release 44 liquinix
toolbox enter liquinix

# inside the toolbox:
sudo dnf install gcc-c++ cmake ninja-build git wayland-devel wayland-protocols-devel \
    layer-shell-qt-devel qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtdeclarative-devel qt6-qtwayland-devel \
    plasma-wayland-protocols-devel  # Qt 6: see docs/linux-roadmap.md
```

`sudo` inside the toolbox only changes the container. If an install goes wrong, start over with
`toolbox rm -f liquinix`.

Then build from the repository root, inside the toolbox (`CMakeLists.txt` builds only the Linux programs; Windows
keeps using `build.cmd`):

```sh
cmake -B build -G Ninja
ninja -C build                 # build/liquinix-dock
```

The programs run on the host too (outside the toolbox), as long as the host has the same Qt 6 and layer-shell-qt
versions, which a Plasma 6 desktop on the same Fedora release does. Shared code lives in `src/core` (no platform
code at all) and `src/platform` (one interface, a `win/` and a `linux/` backend); Linux-only programs are in
`src/linux`.

`tools/linux/ci-smoke.sh ./build/liquinix-dock` runs the same check as the CI's Linux job: a headless KWin
(`kwin_wayland --virtual`, with its own runtime folder and D-Bus, so your desktop isn't touched) and the Dock inside
it, passing only if the Dock got its layer-shell surface and drew a frame.

`liquinix-dock` reads the same `ajustes.txt` keys as the Windows Dock (`tamanho`, `ampliacao`,
`ampliacao_tamanho`) from `$XDG_CONFIG_HOME/liquinix/`, plus `tema_icones` (an icon theme name such as
`MacTahoe`; by default KDE's theme). Its pinned apps are in `dock.txt` there, one `.desktop` id per line, created
on first run. `LIQUINIX_DOCK_PNG=dock.png` draws the Dock into an image and exits, without a window
(`LIQUINIX_DOCK_PNG_MOUSE=<x>` puts the pointer over it); the CI uploads those images with each run. Point
`XDG_CONFIG_HOME` at a scratch folder when testing, so your own `~/.config` stays untouched.

`LIQUINIX_DOCK_SCRIPT="move 501 140; press; move 580 140; release"` (with `QT_QPA_PLATFORM=offscreen`) sends
those mouse events to the Dock, prints the pinned order and exits: a way to test dragging without a screen. A press and
release without moving is a real click and launches the app. `tools/linux/ci-dock-tests.sh ./build/liquinix-dock` runs
the CI's behaviour tests that way (dragging to reorder and unpin, saving `dock.txt`, the launch bounce), with fake apps
that launch nothing.

The Dock marks running apps with a dot and brings an app's window to the front when its icon is clicked, using
Plasma's window-management protocol (`org_kde_plasma_window_management`), matched by `.desktop` id or
`StartupWMClass`. KWin may restrict that protocol to trusted programs; the nested KWin offers it without any setup.
If a session doesn't, the Dock logs `Plasma window list not available yet` and works without the dots.

The shelf is frosted glass when the compositor blurs behind it: the standard `ext-background-effect-v1` protocol,
which KWin 6.8 offers (the older `org_kde_kwin_blur`, which KWindowSystem uses, isn't offered any more). Without it,
or in safe mode, the shelf is opaque. The Dock logs `blur available` or `blur not available (opaque shelf)`.

## 2. Run in a nested KWin

`tools/linux/nested-kwin.sh` opens a second KWin as a window on your desktop. Programs started inside it talk to
that KWin instead of your real one. It has the protocols a desktop shell needs (layer-shell, KDE's plasma-shell,
background blur), so the dock and menu bar can be tested there. If they crash, freeze or grab input, only that
window is affected.

```sh
tools/linux/nested-kwin.sh                          # opens a 1600x900 nested session
WAYLAND_DISPLAY=wayland-liquinix ./build/liquinix-dock     # from another terminal
```

You can also pass the program directly: `tools/linux/nested-kwin.sh ./build/liquinix-dock`. Set `WIDTH`,
`HEIGHT` and `SCALE` to test other screen sizes and HiDPI (`SCALE=2`).

A debugger works the same way: `WAYLAND_DISPLAY=wayland-liquinix gdb ./build/liquinix-dock`.

What the nested session is not good for: animation smoothness, blur quality, and CPU and GPU use. It renders
through your real session, so it's slower and simpler. Measure performance in the test account (section 3).

To check what the nested KWin offers:
`WAYLAND_DISPLAY=wayland-liquinix wayland-info | grep -E 'layer_shell|plasma_shell|background_effect'`.

## 3. Test the real desktop in a second account

Create a separate account for testing Liquinix as a real desktop, at login and next to (or instead of) Plasma's
panel:

```sh
sudo useradd -m -c "Liquinix test" liquinix-test
sudo passwd liquinix-test
```

Use **Switch User** in the application launcher (or the lock screen) to log into it while your own session keeps
running. Give the test account access to the build, for example by installing to its `~/.local` from your
build folder, or by cloning the repository there.

This account is where you turn on autostart, replace the Plasma panel, and measure performance (idle CPU, frame
rate), because it's a real session on the real GPU. If it breaks, switch back to your account and fix it from
there. Your own desktop and settings are never touched.

## 4. A virtual machine for root-level work

Use a VM for anything that needs root or changes how the system starts:
- running your own compositor as the session;
- changes to the display manager (SDDM) or session files;
- testing RPM or Flatpak packages.

virt-manager or GNOME Boxes with a Fedora KDE image works well (`sudo dnf install virt-manager`). Graphics in a
VM are slow, so don't use it for performance work.

To test the Windows side from Linux, see `docs/windows-vm.md`.

## Safety nets

**Kill switch.** `tools/linux/kill-liquinix.sh` ends every process whose name starts with `liquinix`, and
force-kills anything still running after 2 seconds. Bind it to a shortcut (for example Meta+Ctrl+Alt+Esc) in
System Settings > Keyboard > Shortcuts > Add New > Command or Script. It matches process names, not command
lines, so it won't kill your editor or terminal just because the repository folder is called Liquinix.

**Safe mode.** The Linux apps should check `LIQUINIX_SAFE_MODE=1` and start with optional features off
(autostart, global shortcuts, input grabs, blur), so a bad setting can always be bypassed.

**Snapshots.** If your root filesystem is Btrfs (`findmnt -no FSTYPE /`), take a Timeshift (or snapper) snapshot
before installing packages or changing anything outside your home folder. It's the undo button for system-level
mistakes.

**Getting out of a broken session.**
- **The desktop froze but the system didn't:** press Ctrl+Alt+F3 for a text console, log in, and run
  `tools/linux/kill-liquinix.sh` (or delete the autostart entry in `~/.config/autostart/`). Return to the desktop
  with Ctrl+Alt+F1 or F2.
- **Plasma's own panel or desktop disappeared:** run `plasmashell --replace &` from KRunner (Alt+Space) or a
  terminal.
- **Login itself fails:** pick another user, or another session in the login screen, and undo from there.

## Continuous integration

Later, the GitHub Actions Linux job can run KWin without a screen (`kwin_wayland --virtual --socket
wayland-ci`), start the apps inside it and take screenshots. That runs next to the existing Windows build.

## GNOME

The same layers apply on GNOME, with these differences:
- A nested GNOME Shell runs in a window too: `gnome-shell --devkit` on recent GNOME, `gnome-shell --nested` on
  older versions. Check `gnome-shell --help` for the one your version has (not tested here; this guide was
  written on KDE).
- GNOME Shell does not support layer-shell. Docks and panels there need a GNOME Shell extension, and a broken
  extension can crash the shell. Test extensions in the nested shell or the test account first.
