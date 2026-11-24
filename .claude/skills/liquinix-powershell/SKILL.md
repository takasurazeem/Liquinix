---
name: liquinix-powershell
description: Rules for Liquinix's PowerShell and batch scripts (tools/*.ps1, instalador/*.ps1, build.cmd and other .cmd files) - installer, uninstaller, updater, packaging, release, icon and registry tools. Use when writing or editing any .ps1 or .cmd file in this repo, especially anything that changes the registry, scheduled tasks, files outside the install folder, or what goes into a release.
---

# PowerShell and batch scripts

The installer and tools run on ordinary users' PCs through Windows PowerShell 5.1, often from a double-click,
with no developer tooling. They change the registry, scheduled tasks and file associations. A broken script means
a half-installed desktop, so compatibility and reversibility matter more than elegance.

## Compatibility: Windows PowerShell 5.1

- No PowerShell 7-only syntax: no `??` or `??=`, no ternary `a ? b : c`, no `&&`/`||` pipeline chains, no
  `ForEach-Object -Parallel`, no `-AsHashtable` on `ConvertFrom-Json`.
- Use `$ErrorActionPreference = 'Stop'` at the top, as the existing scripts do, and handle expected failures with
  `-ErrorAction SilentlyContinue` deliberately, not everywhere.
- Call external tools with explicit paths or `Get-Command` checks (`git`, `gh`, `winget`, `7z`). Fail with a
  clear message when they're missing.

## Encoding

Files with non-ASCII text (most of them: comments and messages are in Portuguese) must keep their **UTF-8 BOM**.
Windows PowerShell 5.1 reads a BOM-less file as the ANSI code page and mangles accents, which can break string
comparisons and paths. Check with `file <script>`, and preserve the BOM and the existing line endings when
editing. When writing text files from a script, pass `-Encoding utf8` (5.1 writes it with a BOM).

## Paths and personal data

- Use `$env:USERPROFILE`, `$env:LOCALAPPDATA`, `$env:APPDATA` and `$PSScriptRoot`. Never hard-code
  `C:\Users\<name>`.
- Liquinix installs per user in `%LOCALAPPDATA%\Programs\Liquinix` without administrator rights. Keep new
  features within HKCU and the user's folders unless they truly need more, and then ask for elevation only for
  that step.

## Reversible by design

- **Logging:** every change to system state (registry, scheduled task, shortcuts, file associations, system
  icons) is written to the install log (`Anota` in `instalador/instalar.ps1`), with the old value.
- **Undo:** `instalador/desinstalar.ps1` restores from that log. If you add a change, add its undo in the same
  pull request.
- **Idempotent:** running the installer twice, or updating over an old version, must not duplicate entries or
  lose the user's settings.
- **Ask first:** prompt (`Pergunta`) before optional or invasive changes, and skip prompts in silent/update
  mode.

## Packaging and releases

`tools/empacotar.ps1` builds the release zip, and `tools/publicar.ps1` publishes it. They must never bundle
Apple's fonts, sounds or cursors, or untracked icons. The final check in `empacotar.ps1` enforces that; don't
weaken it (see the `liquinix-legal-guard` skill). The zip must include `LICENSE.txt`.

## Naming

Keep the existing Portuguese script and function names (`empacotar`, `instalar`, `Anota`, `Pergunta`). Don't
rename them to Verb-Noun: other scripts, docs and the installer refer to them by name.

## Checking

- From Linux, `pwsh -NoProfile -Command "[scriptblock]::Create((Get-Content -Raw <file>)) | Out-Null"` catches
  syntax errors if `pwsh` is installed. It doesn't prove 5.1 compatibility; reread for 7-only syntax.
- On Windows, run scripts with `powershell -NoProfile -ExecutionPolicy Bypass -File <script>`, the way the
  installer does, not from PowerShell 7.
- Say which scripts were not run.
