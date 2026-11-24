# AGENTS.md

Guidance for AI coding agents (Claude, Codex and others) working in this repository.

## Project

Liquinix is a macOS-style desktop that runs on top of Windows 11 (menu bar, Dock, Finder, Spotlight, Control
Center, Liquid Glass and a set of built-in apps), written in native C++: Win32, Direct2D/DirectWrite and
DirectComposition, with no frameworks. Linux support is planned. It is licensed under **GPL-3.0**.

Liquinix is a fork of WacOS by renzyxx, taken from the last GPL-3.0 revision. See `PROVENANCE.md`.

## What to work on

Work is tracked as GitHub issues on the [project board](https://github.com/users/takasurazeem/projects/10).
**`docs/ROADMAP.md` gives the order to pick them in** and a short routine for choosing the next card:
- the first open issue whose dependencies are closed;
- confirmed with the user before starting;
- one branch and one pull request per card, with `Fixes #N`.

When a session starts without a specific task, or the user says "get started" or "next card", follow that file.

## Hard rules

These protect the project legally. Do not break them, even if asked to "just make it work".

1. **No code from upstream WacOS after commit `0afb96a6`.** Later upstream versions are proprietary
   ("source-available, all rights reserved"). Do not fetch, read, copy, paraphrase or port code, shaders, assets or
   config from `github.com/renzyxx/WacOS` beyond that commit. Features upstream added later are reimplemented
   independently from descriptions only (for example `FEATURES_TODO.md`, when present locally), never from their code.
2. **No Apple or unlicensed third-party assets in the repository or in release packages.** That includes SF
   fonts, SF Symbols, macOS icons, wallpapers, sounds and cursors, and icon packs from VIN STAR, macOSicons.com,
   DeviantArt or similar sources. Only assets with a license that allows redistribution (GPL-3.0 compatible) may be
   committed, with credit in a `CREDITOS.txt` next to them. Apple's assets may only be *downloaded on the user's
   own PC* at install time (`tools/baixar-apple.ps1`).
3. **Keep the release package clean.** `tools/empacotar.ps1` ships only tracked files and refuses packages that
   contain fonts, sounds, cursors or untracked icons. Do not weaken or bypass that check.
4. **No personal data.** No account names, home-directory paths (`C:\Users\<name>`), machine backups, registry
   exports, logs or screenshots of a real desktop. Use `%USERPROFILE%` / `$env:USERPROFILE`.
5. **Keep the GPL notices.** Do not change `LICENSE` or remove the WacOS credit in `README.md` and `PROVENANCE.md`.
   New files are GPL-3.0 as part of the project.
6. **Do not use Apple's or WacOS's branding** (logos, the WacOS name) for Liquinix itself.

## Build (Windows)

Requires Visual Studio 2022 or Build Tools 2022 with the C++ workload.

```
build.cmd            everything
build.cmd shell      only LiquinixShell.exe (menu bar, Dock, Spotlight...)
build.cmd finder     only the Finder
build.cmd janelas    Mac title bars for other apps (LiquinixWindow.dll 32/64-bit and LiquinixTitleBars.exe)
build.cmd setup      only LiquinixSetup.exe (install, update, uninstall)
```

`build.cmd` lists every target in its header. Output goes to `bin\` (ignored by git). CI
(`.github/workflows/build.yml`) runs `build.cmd` and `build.cmd janelas` on `windows-2022` for every push to `main`.

Release: `tools\publicar.ps1` (calls `tools\empacotar.ps1 -SemSistema -SemAjustes`, then creates a GitHub release).

There are no automated tests. Check a change by building it, and say clearly when something could not be built
or run (for example, Windows code edited from Linux).

## Layout

- `src/shell`: the shell (menu bar, Dock, Spotlight, Control Center, Mission Control, Liquid Glass).
- `src/finder`, `src/ajustes` (System Settings), `src/monitor` (Activity Monitor) and the other apps in `src/*`.
- `src/janelas`: Mac title bars injected into other apps' windows; `src/titulos`: those title bars without the shell.
- `src/menu`: context menus over the real Windows shell extensions. `src/dialogo`: the Mac Open/Save dialog.
- `src/comum`: shared code (program names in `nomes.h`, language, glass, shadows, SF Symbols drawing).
- `src/setup`: the installer app. `instalador/`: install, update and uninstall scripts.
- `tools/`: build, resource, icon and packaging scripts.
- `icones/tahoe`: the only bundled icons (MacTahoe icon theme by vinceliuice, GPL-3.0). The code also looks for
  other icon folders (`sequoia`, `simbolos`, `extra`...) and falls back to Windows icons and its own drawings when
  they are missing, which is the normal case in this repository.
- `docs/`: design notes from upstream, mostly in Portuguese. `docs/BRIEFING.md` is the most complete overview; it
  predates the fork, so where it mentions removed folders or upstream workflows, the code and this file win.

## Conventions

- Much of the code, comments, file names and settings keys are in **Portuguese** (`ajustes` = settings,
  `janelas` = windows, `icones` = icons, `sons` = sounds, `bandeja` = tray). Keep existing names; new comments may
  be in English or Portuguese, matching the file.
- Native C++20 with Win32 and Direct2D; no new frameworks, Electron or web views. Keep the UI thread free of disk,
  registry and process waits, and keep idle CPU near zero.
- Settings live in `%APPDATA%\Shell\ajustes.txt` as `key=value` lines.
- PowerShell scripts must run on Windows PowerShell 5.1; keep the UTF-8 BOM on files that have one.
- User-visible text is written in **Portuguese in the code** and wrapped in `Tr()` (`src/comum/idioma.h`), e.g.
  `Tr(L"Abrir")`. Translations live in `src/comum/idioma_<code>.inc` (Portuguese -> that language). Add every new
  string at least to `idioma_en.inc`; a missing translation falls back to English, then to Portuguese. Format
  strings keep the same `%d`/`%ls` in the same order. Use `idioma::Date()`/`MonthName()`/`DayName()` for dates.

## Linux port

Linux support is planned, not started. When working on it, put platform-specific code (windows, drawing, tray,
hotkeys, file icons, the shell integration) behind a platform layer with separate Windows and Linux backends
instead of adding `#ifdef`s through app code, and keep the Windows build passing.

## Skills

Detailed procedures live in project skills under `.claude/skills/` (Claude Code loads them when a task matches;
other agents can read the `SKILL.md` files directly):

| Skill | Use it for |
|---|---|
| `liquinix-legal-guard` | assets, code from elsewhere, upstream features, packaging, before commits and PRs |
| `liquinix-performance` | animations, timers, threads, idle CPU, stutter |
| `liquinix-build-verify` | which `build.cmd` target to build, how to verify, honest reporting |
| `liquinix-translations` | user-visible text, the `Tr()` system, dates |
| `liquinix-win32-ui` | Direct2D, Composition, glass, title bars, themes |
| `liquinix-hooks-injection` | the injected DLL, input hooks, DLL loading, `WM_COPYDATA` |
| `liquinix-powershell` | installer, packaging and tool scripts |
| `liquinix-platform-layer` | the Linux port and any cross-platform work |

`CLAUDE.md` only imports this file, so Claude Code and other agents read the same rules.

## Git

- Commit only when asked. Work on a branch for anything non-trivial.
- Never rewrite published history on `main`.
- `FEATURES_TODO.md` is intentionally untracked; do not commit it.
