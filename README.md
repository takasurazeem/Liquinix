<div align="center">

<img src="docs/logo-256.png" alt="Liquinix logo" width="128" height="128">

# Liquinix

**macOS on top of Windows 11: menu bar, Dock, Finder, Spotlight, Control Center and Liquid Glass, in native C++.**

[![Stars](https://img.shields.io/github/stars/takasurazeem/Liquinix?style=flat&logo=github&label=Stars)](https://github.com/takasurazeem/Liquinix/stargazers)
[![Forks](https://img.shields.io/github/forks/takasurazeem/Liquinix?style=flat&logo=github&label=Forks)](https://github.com/takasurazeem/Liquinix/network/members)
[![Downloads](https://img.shields.io/github/downloads/takasurazeem/Liquinix/total?style=flat&label=Downloads&color=brightgreen)](https://github.com/takasurazeem/Liquinix/releases)
[![Latest release](https://img.shields.io/github/v/release/takasurazeem/Liquinix?style=flat&label=Release)](https://github.com/takasurazeem/Liquinix/releases/latest)
[![Build](https://img.shields.io/github/actions/workflow/status/takasurazeem/Liquinix/build.yml?branch=main&style=flat&label=Build)](https://github.com/takasurazeem/Liquinix/actions/workflows/build.yml)
![Windows 11](https://img.shields.io/badge/Windows-11-0078D4?style=flat&logo=windows11&logoColor=white)
![C++](https://img.shields.io/badge/C%2B%2B-native-00599C?style=flat&logo=cplusplus&logoColor=white)

[![Download Liquinix](https://img.shields.io/badge/DOWNLOAD-Liquinix_for_Windows_11-0A84FF?style=for-the-badge&logo=windows11&logoColor=white)](https://github.com/takasurazeem/Liquinix/releases/latest)

</div>

Liquinix runs on top of Windows (same kernel, drivers and apps) and replaces what you see and touch: the menu bar,
the Dock, Finder, Spotlight, Control Center, Notification Center, Mission Control, Launchpad, window title bars and a
set of built-in apps. Pick the new macOS 27 **Golden Gate** look, the macOS Tahoe look with Liquid Glass, the
macOS Sequoia look, or the classic **Mac OS X Leopard** (2007) Aqua look.

Everything is native C++ (Win32, Direct2D, DirectComposition). No Electron, no web views. It uses about 750 MB of RAM
and under 1% CPU on a 3440×1440 desktop.

**Contents:** [Themes](#themes) · [Features](#features) · [Install](#install) · [Is it safe?](#is-it-safe) ·
[Roadmap](#roadmap) · [Build](#build) · [License](#license)

## Themes

Screenshots were removed because they showed Apple wallpapers and icons that Liquinix cannot
redistribute. New ones will be added as Liquinix gets its own artwork.

Four themes, in System Settings > Appearance > Theme. Switching themes never changes your own wallpaper.

### 1. macOS 27 Golden Gate (new)

The newest look, matched to Apple's macOS 27 screenshots: frosted Liquid Glass on windows, menus and the Dock,
unified toolbars, the macOS 27 icons and the Golden Gate wallpaper.

| | |
|---|---|
| **Quick Look** plays video and music, with a frosted header and an animated open/close | **Photos** with search |
| **Finder** | **System Settings** > Appearance, for every theme |
| **Notes** | **Calendar** |
| **Reminders** | **Music** with search |
| **Clock** with sunrise and sunset times | **Weather** |
| **Activity Monitor** | **Preview** |
| **Calculator** | **Terminal** |
| **TextEdit** | |

Also new with this theme: QuickTime Player with a frosted title bar over the video, the option to hide desktop icons,
**Edit Widgets…** on the desktop menu, clicking the empty strip beside the Dock to show the desktop, Win+D handled by
the shell, grouped notifications that open on click, and smoother glass over animated wallpapers.

### 2. macOS Tahoe (Liquid Glass)

The macOS 26 look: clear Liquid Glass with real refraction and chromatic aberration.

| | |
|---|---|
| **Control Center** in Liquid Glass, with real refraction and chromatic aberration | **Notification Center** with widgets |
| **Spotlight** | **Applications** (Spotlight's Apps mode, with categories) |
| **Finder** | **System Settings** |
| **Activity Monitor** | **Force Quit Applications** (Ctrl+Alt+Esc) |
| Windows message boxes become **macOS alerts** | **Dock icon styles**: Default, Dark, Clear and Tinted |

### 3. macOS Sequoia

The same shell with the macOS 15 look (System Settings > Appearance > Theme).

| | |
|---|---|
| **Control Center** | **Notification Center** |
| **Launchpad** | **Spotlight** |
| **Finder** | **System Settings** with the Sequoia wallpapers |

### 4. Mac OS X Leopard (new)

The 2007 Aqua look, drawn from scratch (nothing reused from the modern themes): the glossy 3D Dock shelf with
reflections, the translucent menu bar, gel buttons, brushed-metal windows, Cover Flow in Finder, System Preferences
laid out like 10.5, Quick Look as a dark HUD, Dashboard, Spaces, Time Machine and the Leopard cursor (your previous cursor comes back when you switch
themes).

| | |
|---|---|
| **Finder** with Cover Flow | **System Preferences** |
| **Quick Look** HUD | **About This Mac** with your PC's real specs |

## Features

- **Menu bar** with the active app's real menus, the Apple menu, status icons and the system tray.
- **Dock** with magnification, running indicators, stacks (Downloads), minimized windows and the Trash.
- **Finder** (file manager), **Spotlight** (search with Applications, Files, Actions and Clipboard modes),
  **Control Center**, **Notification Center**, **Mission Control**, **Launchpad / Apps**, **Quick Look**,
  **Stage Manager** and **Hot Corners**.
- **Mac title bars** (traffic lights, rounded corners, shadows) on other apps' windows.
- **macOS alerts** in place of Windows message boxes, and the Mac Open/Save panel through Finder.
- **Built-in apps**: System Settings, Activity Monitor (with memory pressure), Terminal, Calculator, Notes,
  Reminders, Calendar, TextEdit, Preview, Photos, Music, QuickTime Player, Clock, Weather, Contacts,
  Time Machine, Screenshot and Force Quit.
- **Memory management like macOS**: memory pressure, App Nap and purgeable memory.
- **Themes**: macOS 27 Golden Gate, macOS Tahoe, macOS Sequoia and Mac OS X Leopard.
- **Dynamic Island** (optional, System Settings > Control Center): music, timers, notifications and volume in a pill
  or a notch at the top of the screen.
- **Game Mode** like macOS: when a game comes to the front, Liquinix turns off what costs the most (real glass, mouse
  effects, Hot Corners, file indexing) until the game quits. System Settings > Energy.
- **Launchpad**: hide any app (right-click > Hide from Launchpad), or all of Windows' system tools at once.
- **Icon styles** (Default, Dark, Clear, Tinted), accent colors, light and dark appearance.
- **English and Portuguese** interface (English is the default; System Settings > General > Language).

## Install

1. Download the `Liquinix-<date>.zip` from the [latest release](https://github.com/takasurazeem/Liquinix/releases/latest).
2. Extract it anywhere and open **`LiquinixSetup.exe`**: one app that installs, updates, changes and uninstalls Liquinix,
   in the style of the macOS Installer.

Apple's fonts, sounds and cursors can't be shipped in this repository, so the installer offers to fetch them on
your own PC, from where their owners publish them:

- **SF Pro and SF Mono** from [developer.apple.com/fonts](https://developer.apple.com/fonts/) (Apple's license applies).
  The `.dmg` is opened with 7-Zip, installed through `winget` if missing.
- **macOS alert sounds** from [extratone/macOSsystemsounds](https://github.com/extratone/macOSsystemsounds), converted to WAV.
- **macOS Tahoe cursor** by [jimmyxd2 on DeviantArt](https://www.deviantart.com/jimmyxd2/art/1208233550): the installer opens
  the page, you download it, and it picks the file up from your Downloads folder.

Liquinix installs for your user only (no administrator rights) in `%LOCALAPPDATA%\Programs\Liquinix`.
Everything it changes is logged in `instalacao.log` there. To uninstall, use **Settings > Apps > Installed apps >
Liquinix > Uninstall** (or open `LiquinixSetup.exe` again). The old `instalar.cmd` / `desinstalar.cmd` scripts are still in
the package for advanced use.

- Quit Liquinix: `Ctrl+Alt+Shift+F12`. Emergency exit (brings Explorer back): `Ctrl+Alt+Shift+F11`.

### Use only the apps, or only the title bars

Have your own setup? `LiquinixSetup.exe` asks what to install:

1. **Full Liquinix** (menu bar, Dock, Finder...).
2. **Only the apps you pick** (Finder, Notes, Calculator, Music, Preview...): shortcuts go to a **Liquinix** folder in
   the Start menu. Each app draws its own Mac title bar, with or without the shell running.
3. **Only the Mac title bars** on your other apps: `LiquinixTitleBars.exe`, with an optional start at sign-in.
   Turn it off with `LiquinixTitleBars.exe --sair` (it also stops when the full shell starts, which takes over).
4. **Apps + title bars**.

Options 2 to 4 keep the Windows taskbar, Explorer and your folders as they are, and don't add the shell's sign-in
task. Themes and language are read from System Settings (install it as one of the apps to change them).
Apps that should never get the Mac title bar go one per line in `%APPDATA%\Shell\janelas-excluidas.txt`.
The built-in apps keep the window corners Windows draws, so mods that change the corner radius (like Windhawk's
rounded corners) should no longer leave a square frame around them.

## Updates

New versions are published as numbered releases (`build-N`). In **System Settings > General > Software Update**,
Liquinix checks GitHub on its own, shows the installed version, the latest one and the commits you don't have yet, and
**Update Now** opens `LiquinixSetup.exe`, which downloads the new package, checks its SHA-256 against the one GitHub
publishes and installs it (only programs, icons and sounds are replaced; your settings stay).
GitHub Actions checks that every commit to `main` builds.

## Is it safe?

Fair question for anything that replaces your taskbar. You don't have to trust the author:

- **Open code.** Every file is in this repository, and you can build it yourself (see [Build](#build)).
- **Network.** Liquinix only talks to the weather service (`api.open-meteo.com`, for the weather widget), Google search
  suggestions (only while you type in Spotlight) and GitHub (to check for updates). No telemetry, no accounts.
  Run it behind a firewall like simplewall or GlassWire and watch for yourself.
- **What it installs.** One scheduled task to start at sign-in, with normal user rights. No services, no drivers,
  no administrator rights. Uninstalling (Installed apps > Liquinix) removes everything.
- **Antivirus.** A shell has to do things that look suspicious to heuristics: start at sign-in, use keyboard and
  mouse hooks for shortcuts like the Win key and hot corners, and load its title bar DLL into other apps. The
  build-12 package shows [1/66 on VirusTotal](https://www.virustotal.com/gui/file/310ab4f16824500a8d77225fef66a62e499c32d6d5484a8752b119b7f24e67f3),
  a single AI-based heuristic. The install scripts are plain PowerShell you can read: no inline C#, no hidden
  windows, and the updater checks the package's SHA-256 against the one GitHub publishes. If Microsoft Defender flags it on your PC, it's a false positive that has been
  reported to Microsoft; please [open an issue](https://github.com/takasurazeem/Liquinix/issues) with the threat name.

## Roadmap

- **Dynamic Island**: more activities (inspired by [Atoll](https://github.com/Ebullioscopic/Atoll); no code copied).
- **Signed releases**, so antivirus heuristics stop flagging the installer scripts.
- Your idea? [Open an issue](https://github.com/takasurazeem/Liquinix/issues).

## Build

Requires Visual Studio 2022 (or Build Tools 2022) with the C++ workload.

```
build.cmd            everything
build.cmd shell      the shell only (LiquinixShell.exe: menu bar, Dock, Spotlight...)
build.cmd finder     Finder only
build.cmd janelas    Mac title bars for other apps (LiquinixWindow.dll, 32 and 64 bit, and LiquinixTitleBars.exe)
build.cmd titulos    LiquinixTitleBars.exe only (the title bars without the shell)
tools\empacotar.ps1  the installer package (dist\Liquinix-<date>.zip)
```

## Layout

- `src/shell`: the shell (menu bar, Dock, Spotlight, Control Center, Mission Control, Liquid Glass...).
- `src/finder`, `src/ajustes` (System Settings), `src/monitor` (Activity Monitor) and the other apps in `src/*`.
- `src/janelas`: Mac title bars injected into other apps' windows; `src/titulos`: the title bars without the shell.
- `src/menu`: context menus on top of the real Windows shell extensions.
- `src/comum`: shared code (names, language, glass, shadows).
- `instalador`: install, update and uninstall scripts. `tools`: build and packaging tools.
- `icones`: icon packs (Sequoia, Tahoe) and SF Symbols.

## Performance

Measured on the author's PC (i9-14900K, RTX 3070 Ti, 3440×1440 at 174 Hz). At rest the shell uses about 1% of one
CPU core and no GPU time; with a game in front (Game Mode on) about 2% of one core and no GPU time. Memory: about
350 to 500 MB for the shell (most of it is the Spotlight file index), plus around 100 MB for the hidden Finder that
makes Finder windows open instantly. Animations run on the display's refresh clock, not timers, and nothing polls while
idle.

## License

Liquinix is a fork of [WacOS](https://github.com/renzyxx/WacOS) by renzyxx, based on commit
`0afb96a69ae2fd130621de41a74f1880aa1a9271`, the last version of WacOS published under the GNU General Public
License v3.0. Liquinix is an independent project and is not affiliated with or endorsed by the WacOS author.

The Liquinix source code is released under the [GNU General Public License v3.0](LICENSE): you can use, study, change
and share it, and if you distribute a modified version, its source code has to be released under the GPL too.
Liquinix does not bundle Apple's icons, fonts, sounds or wallpapers. The only bundled icons are from vinceliuice's
[MacTahoe icon theme](https://github.com/vinceliuice/MacTahoe-icon-theme) (GPL-3.0), in `icones/tahoe`.
Anything the installer downloads (fonts, sounds, cursors) stays under its owner's license.
See [PROVENANCE.md](PROVENANCE.md) for how Liquinix was forked from WacOS.

## Note

Apple, macOS, Finder and related names and icons belong to Apple Inc. This is a personal, non-commercial project
and is not affiliated with Apple.
