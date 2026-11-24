---
name: liquinix-win32-ui
description: Conventions for Liquinix's native Windows UI code: Direct2D/DirectWrite drawing, DirectComposition and Windows.UI.Composition glass, WIC images, DWM attributes, the small app2d.h-based apps, Mac title bars, shadows and corners. Use when creating or changing any window, control, drawing, glass/blur, icon rendering or theme (light/dark, Tahoe, Sequoia, Leopard) in src/, or when a window flickers, draws black, loses its glass, or has wrong corners.
---

# Native UI in Liquinix

Everything is C++20 on raw Win32, Direct2D/DirectWrite and DirectComposition / Windows.UI.Composition. There are
no frameworks, no XAML islands for app UI, no Electron and no web views. Keep it that way: the whole point of the
project is a desktop that costs almost nothing. Before editing a file, read the comment at its top. Each .cpp
explains its decisions (in Portuguese), and they are usually there because the obvious approach failed.

## Style to match

- **COM objects** are raw pointers with manual `Release()`, as in the rest of the code (`src/comum/app2d.h`).
  Don't introduce WIL, ComPtr or new helper libraries into a file that doesn't use them; consistency matters more
  in a codebase this size. Release on every path, including errors.
- **Comments** explain *why* and match the file's language (most files are in Portuguese).
- **Logging:** `Log(L"...")` at key points (`src/shell/shell.h:24`); logs go to `bin\*.log`.
- **User-visible text** goes through `Tr()` (see the `liquinix-translations` skill).
- **Settings** come from `%APPDATA%\Shell\ajustes.txt` (`key=value`), read with `shell::Setting(L"key",
  L"default")`. After writing, broadcast the registered message `ShellAjustesMudaram`.
  `docs/BRIEFING.md` section 7 lists the keys and the other registered messages.

## Direct2D

- **Small apps** (Reminders, Calendar and similar) build on `src/comum/app2d.h`. It provides factories, the font
  chain (SF Pro, then Inter, then Segoe), light/dark theme, text helpers, an EDIT control overlay for typing,
  the data file in `%APPDATA%\Shell`, and the Mac title bar through `barra_mac.h`. Start a new app from it instead
  of from scratch.
- **Handle device loss.** When `EndDraw()` returns `D2DERR_RECREATE_TARGET`, release the render target and every
  device-dependent resource, then invalidate (see `app2d.h:235`). Otherwise the window turns black after a
  driver update, a GPU switch on a hybrid laptop, or a remote session.
- **Per-monitor DPI** v2 is on. Scale from the window's DPI, never from a fixed 96.

## Glass and Composition

- The blurred-glass helpers live in `namespace backdrop` (`src/shell/shell.h:526`, `src/shell/vidro_wuc.h`,
  `src/shell/backdrop.cpp`).
- **Real glass** captures windows with Windows.Graphics.Capture. It has to fall back cleanly when capture is
  refused or a capture comes back black (`rg::semCaptura` in `backdrop.cpp`), on games (Game Mode), and with
  `reduzir_transparencia=1`.
- Every glass surface needs a non-glass fallback (a solid tinted fill). Test both.
- **Known Windows limits** (`docs/problemas-conhecidos.md`):
  - `SetWindowRgn` doesn't clip DirectComposition content.
  - Acrylic adds its own frosted layer.
  - Windows rounds top-level window corners at 8 px.

  Work with these limits instead of rediscovering them.

## Title bars, shadows, corners, themes

- Mac title bars in Liquinix's own apps come from `LiquinixWindow.dll`, loaded through `src/comum/barra_mac.h`.
  The traffic-light buttons are in `src/comum/semaforo.h`.
- Shadows: `src/comum/sombra.h`. Theme corner radii: `TemaTahoe` and the other themes in
  `src/janelas/macjanela.cpp`.
- **Themes:** Tahoe/Golden Gate (Liquid Glass), Sequoia, and Leopard (`src/comum/leopard.h`, only with
  `liquid_glass=0`). A visual change usually needs a decision for each theme and for light/dark.
- Draw glyphs and buttons in code when possible. Don't embed bitmaps captured from macOS: those are Apple's
  artwork (see the `liquinix-legal-guard` skill).

## Checking API usage

Many APIs here are undocumented or barely documented:
- `SetWindowCompositionAttribute`
- the virtual-desktop interfaces in `src/shell/mesas.cpp`
- DWM cloaking

For documented APIs, check signatures and threading rules against Microsoft Learn rather than memory; the
Microsoft Learn MCP/plugin helps if installed. For undocumented ones, keep the existing guards and version checks,
and log failures instead of crashing.
