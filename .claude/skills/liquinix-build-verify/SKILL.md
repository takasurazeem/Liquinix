---
name: liquinix-build-verify
description: How to build and verify a Liquinix change, and how to report it honestly. Use after changing any C++, header, .inc, .rc, PowerShell, .cmd or build file, before saying a change works or is done, when choosing which build.cmd target covers a file, when CI fails, and whenever you are working from Linux or macOS where the Windows code cannot be compiled or run.
---

# Build and verify Liquinix

There are no automated tests, so "it compiles and I watched it work" is the only proof. When you couldn't do
that, the honest report is what lets the user trust the rest.

## Which target to build

`build.cmd [target]` at the repo root. It uses MSVC from Visual Studio 2022 or Build Tools 2022, with no MSBuild
and no .vcxproj. Flags: `/O2 /W3 /std:c++20 /utf-8 /EHsc /DUNICODE /D_UNICODE`. The full list of targets is in
the header of `build.cmd`.

| Changed files | Target |
|---|---|
| `src/shell/*` | `shell` |
| `src/finder/*` | `finder` |
| `src/ajustes/*` | `ajustes` |
| `src/janelas/*`, `src/titulos/*` | `janelas` (builds the 64-bit and 32-bit DLLs and the 32-bit host; **not** part of the default build) |
| `src/dialogo/*` | `dialogo` (not part of the default build) |
| `src/setup/*` | `setup` |
| `src/menu/*` (context menus) | no target (default build): compiled into the shell, Finder and System Settings; `menu` builds only the `MenuTeste.exe` test bench |
| `src/comum/*` (shared headers) | no target (default build): every app includes them; also build `janelas` if `barra_mac.h`, `semaforo*.h` or `sombra.h` changed |
| `tools/recursos.ps1`, `src/comum/nomes.h` | no target (default build): names, icons and version resources of every exe |

CI (`.github/workflows/build.yml`) runs only `build.cmd` and `build.cmd janelas` on `windows-2022`. A target
outside those two isn't checked by CI, so build it yourself or say it wasn't built.

## On Windows

1. **Check for another build.** Stop if `cl.exe` or `link.exe` is already running (shared PDBs collide).
2. **Free locked executables.** A running exe can't be overwritten, but it can be renamed. Move only the locked
   ones out of `bin\` first.
   - Close the Finder's hidden standby process with `WM_CLOSE` to the window titled `​ArquivosReserva`.
3. **Build the target.** Read the output for `error` and `fatal`; warnings at `/W3` are worth reading too.
4. **Check the new exe exists, then restart the shell:**
   `PostMessage(FindWindowW(L"ShellMensagens", nullptr), RegisterWindowMessageW(L"ShellReiniciar"), 0, 0)`.
   Never restart without the new exe in place: the shell dies and the desktop is left empty.
5. **Verify the behaviour, not just the build.** Use logs in `bin\*.log` (`shell.log`, `arquivos.log`…), a
   screenshot, a screen recording for flicker, or a measured number (see the `liquinix-performance` skill).
   `bin\ferramentas\MenuTeste.exe --demo <folder>` opens and measures a context menu without clicking.

Changes to system state (registry, shell replacement, icons) need an undo path. Never register Liquinix as the
Windows shell (`Winlogon\Shell`) on a real PC: use a VM.

## Packaging changes

After touching `tools/empacotar.ps1`, run `tools\empacotar.ps1 -SemSistema -SemAjustes` and list the zip:
- It must contain `LICENSE.txt` and `CREDITS.md`.
- It must not contain `fontes`, `sons`, `cursor` or `icones-sistema` folders, or `.ttf`/`.otf`/`.wav`/`.cur`
  files.

## From Linux or macOS

You can't compile or run the Windows code here. Don't claim it builds.
- Read the code paths you changed carefully, and check every Win32 API against Microsoft's documentation
  (signatures, return values, which thread it may be called on).
- Python tools in `tools/` can run here with `python3 -I`. PowerShell scripts can be syntax-checked with `pwsh`
  if installed.
- Push to a branch and let CI build it; check the result with `gh run list` / `gh run view`.
- To run it and check how it looks, use the Windows 11 VM in `docs/windows-vm.md`. Say what was checked there
  and what still needs a real PC (performance, glass and blur).

## Reporting

End with what changed, what was built (which targets), what was run and how it was verified, with numbers where
relevant. Then list what was **not** built, run or tested, plainly. "Not built: edited from Linux; CI will build
`shell` but not `dialogo`" is a good line.
