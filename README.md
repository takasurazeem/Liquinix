<div align="center">

# Liquinix

**A macOS-style desktop for Windows 11 (menu bar, Dock, Finder, Spotlight, Control Center and Liquid Glass) in
native C++. Free software under the GPL-3.0. A Linux version (KDE Plasma) has started.**

[![Build](https://img.shields.io/github/actions/workflow/status/takasurazeem/Liquinix/build.yml?branch=main&style=flat&label=Build)](https://github.com/takasurazeem/Liquinix/actions/workflows/build.yml)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue?style=flat)](LICENSE)
![Windows 11](https://img.shields.io/badge/Windows-11-0078D4?style=flat&logo=windows11&logoColor=white)
![Linux: early](https://img.shields.io/badge/Linux-early-orange?style=flat&logo=linux&logoColor=white)
![C++](https://img.shields.io/badge/C%2B%2B-native-00599C?style=flat&logo=cplusplus&logoColor=white)

</div>

Liquinix runs on top of Windows (same kernel, drivers and apps) and replaces what you see and touch: the menu bar,
the Dock, Finder, Spotlight, Control Center, Notification Center, Mission Control, Launchpad, window title bars and a
set of built-in apps. It has four looks: macOS 27 **Golden Gate**, macOS Tahoe with Liquid Glass, macOS Sequoia, and
the classic **Mac OS X Leopard** Aqua look.

Everything is native C++ (Win32, Direct2D, DirectComposition). No Electron, no web views.

Liquinix is a fork of [WacOS](https://github.com/renzyxx/WacOS) by renzyxx, taken from its last GPL-3.0 version (see
[License](#license)). A Linux version for KDE Plasma has started. See [Linux](#linux).

**Contents:** [Status](#status) · [Themes](#themes) · [Features](#features) · [Install](#install) ·
[Is it safe?](#is-it-safe) · [Linux](#linux) · [Roadmap](#roadmap) · [Build](#build) · [License](#license)

## Status

- **Windows:** the shell and apps work and build on every commit (see the Build badge). The first release,
  [build-1](https://github.com/takasurazeem/Liquinix/releases/latest), is out. See [Install](#install).
- **Linux:** early. The Dock runs on KDE Plasma 6 (Wayland), built from source. See [Linux](#linux).

Work is tracked on the [project board](https://github.com/users/takasurazeem/projects/10), in the order given in
[`docs/ROADMAP.md`](docs/ROADMAP.md).

## Themes

There are no screenshots yet. The old ones showed Apple wallpapers and icons that Liquinix can't redistribute, so
they were removed. New ones will come with Liquinix's own artwork.

Pick a theme in System Settings > Appearance > Theme. Switching themes never changes your own wallpaper.

1. **macOS 27 Golden Gate:** frosted Liquid Glass on windows, menus and the Dock, and unified toolbars in the
   built-in apps (Finder, System Settings, Photos, Music, Notes, Calendar, Reminders, Clock, Weather, Preview,
   Calculator, Terminal, TextEdit, Activity Monitor and Quick Look).
2. **macOS Tahoe:** clear Liquid Glass with refraction and chromatic aberration in Control Center, Notification
   Center, Spotlight and the Dock, with Default, Dark, Clear and Tinted Dock icon styles.
3. **macOS Sequoia:** the same shell with the macOS 15 look.
4. **Mac OS X Leopard:** the 2007 Aqua look, drawn in code. It has the glossy 3D Dock shelf, the translucent menu
   bar, gel buttons, brushed-metal windows, Cover Flow in Finder, System Preferences laid out like 10.5, Quick
   Look as a dark HUD, Dashboard, Spaces and Time Machine.

Liquinix doesn't include Apple's wallpapers, icons, fonts, sounds or cursors. The only bundled icons are from the
[MacTahoe icon theme](https://github.com/vinceliuice/MacTahoe-icon-theme) (GPL-3.0). Liquinix uses wallpapers from
your own `Pictures\Wallpapers macOS` folder when you have them, and the installer can fetch Apple's fonts, sounds
and a cursor onto your own PC (see [Install](#install)).

## Features

- **Menu bar** with the active app's real menus, the Apple menu, status icons and the system tray.
- **Dock** with magnification, running indicators, stacks (Downloads), minimized windows and the Trash.
- **Finder** (file manager), **Spotlight** (search with Applications, Files, Actions and Clipboard modes),
  **Control Center**, **Notification Center**, **Mission Control**, **Launchpad / Apps**, **Quick Look**,
  **Stage Manager** and **Hot Corners**.
- **Mac title bars** (traffic lights, rounded corners, shadows) on other apps' windows.
- **macOS alerts** in place of Windows message boxes, and the Mac Open/Save panel through Finder.
- **Built-in apps:**
  - System Settings, Activity Monitor (with memory pressure), Terminal, Calculator and Notes;
  - Reminders, Calendar, TextEdit, Preview, Photos, Music and QuickTime Player;
  - Clock, Weather, Contacts, Time Machine, Screenshot and Force Quit.
- **Memory management like macOS:** memory pressure, App Nap and purgeable memory.
- **Desktop widgets:** World Clock, Weather, Clock, Calendar and Sunrise.
- **Dynamic Island** (optional, System Settings > Control Center): music, timers, notifications and volume in a pill
  or a notch at the top of the screen.
- **Game Mode:** when a game comes to the front, Liquinix turns off what costs the most until the game quits. That's
  real glass, mouse effects, Hot Corners and file indexing. Find it in System Settings > Energy.
- **Icon styles** (Default, Dark, Clear, Tinted), accent colors, light and dark appearance.
- **12 languages:** English (the default), Portuguese, Spanish, French, German, Italian, Polish, Turkish, Russian,
  Japanese, Chinese and Korean. Change it in System Settings > General > Language.

## Install

1. Download `Liquinix-<date>.zip` from the [releases page](https://github.com/takasurazeem/Liquinix/releases).
2. Extract it anywhere and open **`LiquinixSetup.exe`**. It installs, updates, changes and uninstalls Liquinix.

Apple's fonts, sounds and cursors can't be shipped with Liquinix. The installer can fetch them onto your own PC, from
where their owners publish them, and they stay under their owners' licenses. In `LiquinixSetup.exe`, tick them on the
Installation Type page:

- **SF Pro and SF Mono** from [developer.apple.com/fonts](https://developer.apple.com/fonts/). Apple's license
  applies. The installer opens the `.dmg` with 7-Zip, and installs 7-Zip through `winget` if it's missing.
- **macOS alert sounds** from [extratone/macOSsystemsounds](https://github.com/extratone/macOSsystemsounds),
  converted to WAV.
- **A macOS Tahoe cursor** by [jimmyxd2 on DeviantArt](https://www.deviantart.com/jimmyxd2/art/1208233550), only
  through `instalar.cmd` in the package: it opens the page, you download the cursor, and it picks it up from your
  Downloads folder.

Liquinix installs for your user only, without administrator rights, in `%LOCALAPPDATA%\Programs\Liquinix`.
Everything it changes is logged in `instalacao.log` there. To uninstall, use **Settings > Apps > Installed apps >
Liquinix > Uninstall**, or open `LiquinixSetup.exe` again.

- Quit Liquinix: `Ctrl+Alt+Shift+F12`. Emergency exit (brings Explorer back): `Ctrl+Alt+Shift+F11`.

### Use only the apps, or only the title bars

`LiquinixSetup.exe` asks what to install:

1. **Full Liquinix** (menu bar, Dock, Finder...).
2. **Only the apps you pick** (Finder, Notes, Calculator, Music, Preview...). Their shortcuts go into a **Liquinix**
   folder in the Start menu. Each app draws its own Mac title bar, with or without the shell running.
3. **Only the Mac title bars** on your other apps, through `LiquinixTitleBars.exe`, optionally started at sign-in.
   Turn it off with `LiquinixTitleBars.exe --sair`. It also stops when the full shell starts, since the shell takes
   over.
4. **Apps + title bars**.

Options 2 to 4 keep the Windows taskbar, Explorer and your folders as they are. Themes and language are read from
System Settings, so install it as one of the apps to change them. Apps that should never get the Mac title bar go
one per line in `%APPDATA%\Shell\janelas-excluidas.txt`.

### Updates

In **System Settings > General > Software Update**, Liquinix checks this repository's GitHub releases. It shows the
installed and latest versions and the commits you don't have yet. **Update Now** opens `LiquinixSetup.exe`, which
downloads the new package and checks its SHA-256 against the one GitHub publishes. Then it installs it, replacing
only programs, icons and sounds; your settings stay.

## Is it safe?

It's a fair question for anything that replaces your taskbar. You don't have to trust anyone:

- **Open code.** Every file is in this repository, and you can [build it yourself](#build).
- **Network.** Liquinix talks only to:
  - [Open-Meteo](https://open-meteo.com), for weather and city search in Weather, Clock and the widgets;
  - Google search suggestions, only while you type in Spotlight;
  - [Nager.Date](https://date.nager.at), for public holidays in Calendar;
  - GitHub, to check for updates.

  There's no telemetry and no account. Run it behind a firewall and watch for yourself.
- **What it installs.** One scheduled task to start at sign-in, with normal user rights. No services, no drivers,
  no administrator rights. Uninstalling removes everything.
- **Antivirus.** A shell has to do things that look suspicious to heuristics:
  - start at sign-in;
  - use keyboard and mouse hooks for shortcuts like the Win key and Hot Corners;
  - load its title bar DLL into other apps.

  The install scripts are plain PowerShell you can read. If an antivirus flags Liquinix, please
  [open an issue](https://github.com/takasurazeem/Liquinix/issues) with the detection name.

## Linux

The Linux version targets **KDE Plasma 6 on Wayland**, with Qt 6 (the reasons are in
[`docs/linux-roadmap.md`](docs/linux-roadmap.md)). It shares code with the Windows version: the animation curves, the
drawing interface and the Dock's layout are the same code on both.

**The Dock** (`liquinix-dock`) is the first program:
- your pinned apps (from `.desktop` files), with icons from your icon theme, then Downloads and the Trash;
- magnification, the launch bounce, dots under running apps, and a click brings a running app forward;
- drag to reorder or remove, a right-click menu, automatic hiding and frosted glass behind it;
- settings changes apply live.

There's no package yet: build it from source (CMake) and try it in a nested KWin window first.
[`docs/linux-dev.md`](docs/linux-dev.md) covers the setup (a toolbox container, a nested KWin and a test account), so
the work never risks your own desktop session. The menu bar comes next; its plan is being decided in
[#65](https://github.com/takasurazeem/Liquinix/issues/65). The [project board](https://github.com/users/takasurazeem/projects/10)
shows where it stands.

## Roadmap

The order is in [`docs/ROADMAP.md`](docs/ROADMAP.md) and the cards are on the
[project board](https://github.com/users/takasurazeem/projects/10). In short:

- **The first Liquinix release.** The remaining Apple-derived artwork has been replaced, and the release package
  ships only what Liquinix may redistribute.
- **The Linux port**, as described above.
- **Liquinix's own logo and screenshots.**
- Your idea? [Open an issue](https://github.com/takasurazeem/Liquinix/issues).

## Build

### Windows

Requires Visual Studio 2022 or Build Tools 2022 with the C++ workload.

```
build.cmd            everything
build.cmd shell      the shell only (LiquinixShell.exe: menu bar, Dock, Spotlight...)
build.cmd finder     Finder only
build.cmd janelas    Mac title bars for other apps (LiquinixWindow.dll, 32 and 64 bit, and LiquinixTitleBars.exe)
build.cmd setup      LiquinixSetup.exe only (install, update, uninstall)
tools\empacotar.ps1  the installer package (dist\Liquinix-<date>.zip)
```

The header of `build.cmd` lists every target. The output goes to `bin\`. There are no automated tests; GitHub
Actions builds `build.cmd` and `build.cmd janelas` for every pull request.

To work on the Windows side from a Linux PC, use a Windows 11 virtual machine. See
[`docs/windows-vm.md`](docs/windows-vm.md).

### Linux

Nothing to build yet. See [Linux](#linux).

### Layout

- `src/shell`: the shell (menu bar, Dock, Spotlight, Control Center, Mission Control, Liquid Glass...).
- `src/finder`, `src/ajustes` (System Settings), `src/monitor` (Activity Monitor) and the other apps in `src/*`.
- `src/janelas`: Mac title bars injected into other apps' windows. `src/titulos`: the title bars without the shell.
- `src/menu`: context menus on top of the real Windows shell extensions. `src/dialogo`: the Mac Open/Save dialog.
- `src/comum`: shared code (names, languages, glass, shadows). `src/setup`: the installer app.
- `instalador`: install, update and uninstall scripts. `tools`: build, icon and packaging tools.
- `icones/tahoe`: the bundled MacTahoe icons (GPL-3.0).
- `docs`: design notes (mostly in Portuguese), the roadmap and the development guides.
- [`AGENTS.md`](AGENTS.md): the project's rules, also used as guidance for AI coding agents.

Much of the code, comments and file names are in Portuguese, inherited from WacOS. User-visible text is translated
with `Tr()` (`src/comum/idioma.h`).

## License

Liquinix is a fork of [WacOS](https://github.com/renzyxx/WacOS) by renzyxx, based on commit
`0afb96a69ae2fd130621de41a74f1880aa1a9271`, the last version of WacOS published under the GNU General Public
License v3.0. Liquinix is an independent project and is not affiliated with or endorsed by the WacOS author.

The Liquinix source code is released under the [GNU General Public License v3.0](LICENSE). You can use, study, change
and share it, and if you distribute a modified version, its source code has to be released under the GPL too.
Liquinix does not bundle Apple's icons, fonts, sounds or wallpapers. The only bundled icons are from vinceliuice's
[MacTahoe icon theme](https://github.com/vinceliuice/MacTahoe-icon-theme) (GPL-3.0), in `icones/tahoe`.
Anything the installer downloads (fonts, sounds, cursors) stays under its owner's license.
See [PROVENANCE.md](PROVENANCE.md) for how Liquinix was forked from WacOS, and [CREDITS.md](CREDITS.md) for
the projects, data and people Liquinix builds on.

## Note

Apple, macOS, Finder and related names and icons belong to Apple Inc. This is a personal, non-commercial project
and is not affiliated with Apple.
