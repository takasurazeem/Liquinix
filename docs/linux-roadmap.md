# Linux roadmap: the decisions

The choices that shape the Linux port, made in card #17 (November 2026). They are expensive to reverse, so a
change to any of them gets its own issue and an update to this file.

| Question | Decision |
|---|---|
| Toolkit | **Qt 6**, for the whole Linux side |
| First desktop | **KDE Plasma 6** |
| Display server | **Wayland only** |
| First feature end to end | **The Dock** (`liquinix-dock`, issue #20) |

How the code is split between Windows and Linux is in `.claude/skills/liquinix-platform-layer/SKILL.md`. How to
build and run it is in `docs/linux-dev.md`.

## Toolkit: Qt 6

**Why:**
- **It fits the code we have.** The Windows apps draw everything themselves with Direct2D: rectangles, rounded
  shapes, gradients, text and bitmaps, every frame. `QPainter` on a `QWindow` is the same kind of immediate-mode
  2D drawing, so the platform layer can offer one drawing interface with a Direct2D backend and a `QPainter`
  backend. App drawing code doesn't have to be rewritten as widgets or a scene graph.
- **Layer-shell is KDE's own.** `layer-shell-qt` is maintained by KDE and is what Plasma's own panels use, so docks
  and bars get the same treatment as Plasma's.
- **Frame pacing is built in.** On Wayland, `QWindow::requestUpdate()` is driven by the compositor's frame
  callbacks. That's the Linux equivalent of the display clock the Windows apps moved to in #33: no timers for
  animations, and nothing runs when nothing moves.
- **Blur is available.** KWin's blur protocol (`org_kde_kwin_blur`) can be requested for our surfaces, through
  KWindowSystem or the protocol directly, so glass doesn't have to be faked on Plasma.
- **It's already in the dev setup.** The toolbox in `docs/linux-dev.md` installs `qt6-qtbase-devel`,
  `qt6-qtdeclarative-devel` and `layer-shell-qt-devel`.

**License:** Qt 6 is available under LGPL-3.0 and GPL; `layer-shell-qt` is LGPL. Both are fine for a GPL-3.0
project. We link to them dynamically and don't ship them: distributions provide them.

**Rules that follow from it:**
- Use **Qt Core and Qt GUI** (`QWindow`, `QPainter`, `QImage`, `QFont`). Don't use Qt Widgets for the shell, and
  only use Qt Quick where a feature really needs it; decide that per feature in its issue.
- Qt types stay inside the Linux backend. App logic (Dock layout, Spotlight ranking, settings, translations,
  animation curves) keeps using our own types, the same on both platforms.
- Build with **CMake** (`find_package(Qt6 ...)`) next to `build.cmd`, which stays the Windows build (#18).

**Not chosen:**
- **GTK 4:** `gtk4-layer-shell` works on wlroots and KDE, but its integration with KWin (blur, Plasma's own
  protocols) is weaker. It would be the better fit if GNOME came first, but GNOME has no layer-shell either way.
- **No toolkit** (Wayland and Skia or Cairo directly): the most control, but we'd write input, text layout, scaling
  and protocol handling ourselves before drawing anything.

## First desktop: KDE Plasma 6

**Why:** it has layer-shell for docks and bars, a compositor (KWin) with blur and effects, and the Linux dev setup
already targets it: the nested KWin in `tools/linux/nested-kwin.sh` and the test account in `docs/linux-dev.md`.

**Later:** wlroots compositors (Hyprland, Sway and others) also speak layer-shell, so the Dock should mostly work
there. Blur and effects differ per compositor, so treat each as its own card, and fall back to an opaque look where
blur isn't available.

**GNOME is out of scope for now.** It has no layer-shell; a dock or menu bar there would have to be a GNOME Shell
extension written in JavaScript, which is a different project.

## Display server: Wayland only

**Why:** Plasma 6 and most distributions default to Wayland, and layer-shell is a Wayland protocol. Supporting X11
too would need a second way to dock windows and reserve screen space, tested on both.

**What it costs:** Wayland blocks some things Windows allows, on purpose. Use the desktop portals and say plainly
when something can't work the Windows way:
- **Global shortcuts:** the GlobalShortcuts portal or KDE's own shortcuts, not input snooping.
- **Screen capture** (live glass, screenshots): the ScreenCast and Screenshot portals over PipeWire, which ask the
  user first.
- **Other apps' windows:** no injected title bars (`src/janelas` has no Linux version); the compositor draws window
  decorations. The window list for the Dock comes from what KDE exposes (for example the
  `org_kde_plasma_window_management` protocol or KWin scripting), to be decided in #20.

X11 can be reconsidered later as its own issue, if users ask for it.

## First feature: the Dock

`liquinix-dock`, as planned in issue #20: a layer-shell surface anchored to the bottom edge, showing pinned apps
from `.desktop` files with icons from the icon theme (MacTahoe), launching them, and marking running apps.

**Why the Dock:** it's the most visible part of Liquinix, it needs only one layer-shell surface, and it exercises
most of the platform layer at once (window, drawing, icons, launching, settings), so it shows early whether the
split works. The menu bar comes after it: app menus on Wayland have no protocol that works everywhere, so it
needs more decisions first.

## Order of the Linux cards

1. **#19** Extract a platform layer on Windows, with no behaviour change. Can start now.
2. **#18** Toolbox and CMake build skeleton: a Qt 6 hello-world `liquinix-dock` in the nested KWin.
3. **#20** The Dock, end to end.
4. **#21** Linux CI with a headless KWin.

#18 and #19 don't depend on each other. #20 needs both.
