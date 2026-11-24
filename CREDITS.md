# Credits

Liquinix is free software under the GNU General Public License v3.0 (see [LICENSE](LICENSE)). It builds on the
work of the projects and people below. Thank you.

If you see your work here credited wrongly, or missing, please
[open an issue](https://github.com/takasurazeem/Liquinix/issues).

## Upstream

- **WacOS** by renzyxx: <https://github.com/renzyxx/WacOS> (GPL-3.0).
  Liquinix is a fork of commit `0afb96a69ae2fd130621de41a74f1880aa1a9271`, the last revision of WacOS published
  under GPL-3.0. See [PROVENANCE.md](PROVENANCE.md).

## Included in this repository

- **MacTahoe icon theme** by Vince Liuice (vinceliuice):
  <https://github.com/vinceliuice/MacTahoe-icon-theme> (GPL-3.0).
  The app, folder and trash icons in `icones/tahoe/` are PNG renders of its SVGs, made with
  `tools/icones-mactahoe.ps1` (the file-by-file mapping is in `icones/tahoe/CREDITOS.txt`). Brand logos in the
  theme are trademarks of their owners.
- **Windows 11 Settings Styler** (a Windhawk mod) by Michael Maltsev (m417z):
  <https://github.com/ramensoftware/windhawk-mods> (GPL-3.0).
  `docs/windhawk/bloqueio-sequoia/sequoia-lockscreen.wh.cpp` is a modified copy of version 1.2.1, retargeted to
  the Windows lock screen (`LockApp.exe`) with its statistics timer removed. That file also contains:
  - code based on **TranslucentTB** (ExplorerTAP, XamlBlurBrush): <https://github.com/TranslucentTB/TranslucentTB>
    (GPL-3.0);
  - small snippets from Stack Overflow answers by Giovanni Funchal and Jeff T. (CC BY-SA 3.0) and by Arafat Hasan,
    Justin and Phidelux (CC BY-SA 4.0), each linked next to the snippet;
  - built-in Settings themes from the
    [Windows 11 Settings styling guide](https://github.com/ramensoftware/windows-11-settings-styling-guide), by
    eugenesvk, milestprower92, Nimai-HK, FireBlade211, linkvegas12, Nathaniel4JC, wasixgamer and ArvindSaini978.

## Downloaded on your PC by the installer (not redistributed by Liquinix)

The installer offers to fetch these from where their owners publish them (`tools/baixar-apple.ps1`). They are
never part of this repository or of a Liquinix release.

- **SF Pro and SF Mono** fonts by Apple Inc.: <https://developer.apple.com/fonts/> (Apple's font license).
- **macOS alert sounds** by Apple Inc., fetched from
  [extratone/macOSsystemsounds](https://github.com/extratone/macOSsystemsounds), collected by David Blue.
- **macOS Tahoe cursor** by jimmyxd2: <https://www.deviantart.com/jimmyxd2/art/1208233550>.
- **7-Zip** by Igor Pavlov: <https://www.7-zip.org> (LGPL-2.1-or-later, with BSD-3-Clause parts and the unRAR
  restriction), installed through winget to unpack Apple's font disk image.

## Services and runtime dependencies

- **Open-Meteo**: <https://open-meteo.com>. Weather and geocoding data for Weather, Clock and the desktop widget.
  Weather data by Open-Meteo.com, licensed CC BY 4.0.
- **Windhawk** by Ramen Software: <https://windhawk.net> (GPL-3.0). Needed only for the optional mods in
  `docs/windhawk/`, which also configure *Windows 11 File Explorer Styler* by m417z and *Explorer Font Changer* by
  Gabriela Cristei.
- **C++/WinRT** (Microsoft, MIT) and the Windows SDK, used to build Liquinix.
- **Pillow** (MIT-CMU) and **NumPy** (BSD-3-Clause), used by the Python tools in `tools/`.
- **Inter** by Rasmus Andersson (SIL OFL 1.1), used as a fallback font when it is installed.

## Inspiration and references (no code copied)

- **Atoll** by Ebullioscopic: <https://github.com/Ebullioscopic/Atoll> (GPL-3.0), itself derived from
  **boring.notch** by TheBoredTeam: <https://github.com/TheBoredTeam/boring.notch> (GPL-3.0). Reference for the
  Dynamic Island's behaviour and proportions (`src/shell/ilha.cpp`).
- **ReactOS** (GPL-2.0) and **ManagedShell / Cairo Desktop** (Apache-2.0): studied for the Windows
  notification-area protocol (`docs/estudo-bandeja.md`).
- **kube.io**: the article on Liquid Glass refraction (Snell's law over a convex squircle bezel), used for the
  glass in `src/shell/backdrop.cpp`.
- **VirtualDesktop** by Markus Scholtes: <https://github.com/MScholtes/VirtualDesktop> (MIT). Reference for the
  undocumented Windows virtual-desktop interfaces (`src/shell/mesas.cpp`).
- **microsoft-ui-xaml** by Microsoft: <https://github.com/microsoft/microsoft-ui-xaml> (MIT). Reference for XAML
  diagnostics.
- **Seelen UI** and **YASB**: reference for the undocumented `SetWindowCompositionAttribute` API
  (`src/menu/context_menu.cpp`).
- **512 Pixels** by Stephen Hackett, the **toastytech.com** GUI gallery and public videos: screenshots used to
  measure macOS layouts, sizes and colours.

## Trademarks

Apple, macOS, Finder and related names and icons are trademarks of Apple Inc. Other product names and logos are
trademarks of their owners. Liquinix is not affiliated with or endorsed by Apple, by the WacOS author, or by any
project listed above.
