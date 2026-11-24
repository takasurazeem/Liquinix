---
name: liquinix-platform-layer
description: Architecture guide for making Liquinix cross-platform (Windows and Linux). Use when planning or writing any Linux code, adding CMake or another cross-platform build, introducing #ifdef _WIN32 / __linux__, extracting Windows-specific code behind an interface, or choosing Linux technologies (Wayland, X11, GTK, Qt, Cairo, Skia, D-Bus, PipeWire, portals, GNOME/KDE extensions) for a Liquinix feature.
---

# Platform layer for the Linux port

Liquinix is Windows-only today: there are no `_WIN32` or `__linux__` checks anywhere in `src/`. Every app calls
Win32, Direct2D and DirectComposition directly. The goal is Windows **and** Linux without breaking or slowing
down the Windows version, which is the one users run.

## Principles

1. **An interface, not `#ifdef`s through app code.** Put platform services behind small C++ interfaces, with one
   backend per platform, for example `src/plataforma/` with `win/` and `linux/`. App logic (Dock layout,
   Spotlight ranking, settings, translations, animation curves) should not know which backend it runs on.
   `#ifdef` belongs only in the backend selection.
2. **Extract before you port.** First move existing Windows code behind the interface with no behaviour change,
   and keep `build.cmd` and CI green at every step. Only then write the Linux backend. Mixing the two makes
   regressions on Windows impossible to see.
3. **One feature at a time, end to end.** A Linux Dock that works beats a Linux skeleton of everything.
4. **The same performance rules on both** (see the `liquinix-performance` skill): frame callbacks instead of
   timers, no blocking I/O on the main loop, events instead of polling.
5. **Some features don't port.** Injecting title bars into other apps' windows (`src/janelas`) and replacing the
   Windows shell have no direct Linux equivalent. On Linux, window decorations and the desktop belong to the
   compositor or desktop environment. Plan those as a compositor, a GNOME Shell or KWin extension, or simply "not
   on Linux". Don't emulate the Windows mechanism.

## Mapping (starting points; decide per feature and record the choice)

| Windows today | Linux options |
|---|---|
| HWND, window messages | Wayland surfaces (layer-shell for panels and docks: `wlr-layer-shell`, KDE's layer-shell) or X11 windows; or the toolkit's windows |
| Direct2D / DirectWrite | Skia, or Cairo + Pango/HarfBuzz; the toolkit's renderer if using Qt Quick/GTK 4 |
| DirectComposition / Windows.UI.Composition, DWM blur | Compositor effects (KWin blur, Hyprland/wlroots), or render the blur yourself; Qt Quick effects |
| Windows.Graphics.Capture (real glass) | PipeWire + xdg-desktop-portal ScreenCast; restricted by design on Wayland |
| Tray (`Shell_NotifyIcon`, tray protocol) | StatusNotifierItem over D-Bus |
| Global hotkeys (LL keyboard hook) | xdg-desktop-portal GlobalShortcuts; compositor-specific bindings |
| Notifications | `org.freedesktop.Notifications` over D-Bus |
| Shell file icons, file associations | freedesktop icon theme spec, `.desktop` files, shared-mime-info, GIO |
| `%APPDATA%\Shell\ajustes.txt` | `$XDG_CONFIG_HOME/liquinix/ajustes.txt` (keep the same format) |
| Registry, scheduled task autostart | `~/.config/autostart/*.desktop` or a systemd user unit |
| MSVC + `build.cmd` | CMake (or Meson) for Linux alongside `build.cmd`, until one build can serve both |

If you pick a toolkit (Qt or GTK), choose once for the whole Linux side, write down why in `docs/`, and check its
license (Qt is LGPL-3.0/GPL; GTK is LGPL-2.1+). Both are fine for a GPL-3.0 project.

## Before writing Linux code

- **Check the plan with the user.** Which desktop first (GNOME, KDE, wlroots compositors), Wayland-only or X11
  too, and which toolkit. These choices are expensive to reverse.
- **Keep the tray, input and capture code honest.** Wayland deliberately blocks global input snooping and free
  screen capture. Use portals rather than workarounds, and tell the user when something can't work the Windows
  way.
- **Add Linux CI** (an Ubuntu runner) as soon as there is Linux code, next to the Windows job.

## Legal

New Linux code is GPL-3.0 like the rest. Linux icon themes and fonts you bundle must be redistributable (see the
`liquinix-legal-guard` skill). MacTahoe and WhiteSur are GPL-3.0 and are natural fits.
