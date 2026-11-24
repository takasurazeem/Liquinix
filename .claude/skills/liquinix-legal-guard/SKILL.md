---
name: liquinix-legal-guard
description: Licensing and provenance checklist for Liquinix. Use it before adding, generating, converting or replacing any asset (icon, image, font, sound, cursor, wallpaper, screenshot, bitmap table in a .inc file), before implementing a feature that upstream WacOS added after the fork (anything in FEATURES_TODO.md, or "port X from WacOS"), before copying code from any outside project or website, before touching tools/empacotar.ps1, tools/publicar.ps1, LICENSE, README.md, CREDITS.md or PROVENANCE.md, and before committing or opening a pull request. Use it even when the change looks harmless, such as "just add an icon for app X" or "copy the button graphics from a screenshot".
---

# Liquinix legal guard

Liquinix is a GPL-3.0 fork of WacOS taken from upstream commit `0afb96a6`, the last GPL-3.0 revision
(`PROVENANCE.md`). Later upstream versions are proprietary, and the project ships to the public. One copied file
or one Apple icon in a release can mean a takedown of the whole repository, so these checks come before
convenience. `AGENTS.md` lists the hard rules; this skill is how to apply them.

## 1. Code from elsewhere

- **Upstream WacOS after `0afb96a6`: never.** Don't fetch, open, quote or paraphrase anything from
  `github.com/renzyxx/WacOS` beyond that commit, including diffs, issues with code, or release binaries. Features
  upstream added later are rebuilt from their one-line descriptions (`FEATURES_TODO.md`, kept untracked) and your
  own design. If a task says "do it like WacOS does", design it yourself and say so.
- **Other projects:** check the license before copying anything.
  - GPL-3.0, GPL-2.0-or-later, LGPL, MIT, BSD, Apache-2.0, zlib and CC BY-SA 4.0 (snippets) are compatible.
  - Plain GPL-2.0-only, CC BY-NC, "free for personal use", unlicensed code and code from closed products are
    not. Reimplement from documentation instead.
  - Keep the original copyright notice, link the source next to the code, and add the project to `CREDITS.md`.
- **Inspiration without copying** (behaviour, measurements, published maths) is fine. Say "reference only, no
  code copied" in a comment, the way `src/shell/ilha.cpp` does for Atoll.

## 2. Assets

Before adding any image, icon, font, sound, cursor, video, or a pixel table generated into a `.inc` file:

1. **Who made it?** Apple artwork (icons, SF Symbols, SF fonts, wallpapers, sounds, cursors, UI captured from
   screenshots) cannot be in the repository or in a release, whatever the source site says.
   - **Fan-made Apple-style icons** (macOSicons.com, DeviantArt, VIN STAR, "Big Sur icon packs") are usually
     redraws of Apple's icons with no license. Treat them like Apple's.
   - **Bitmaps traced or extracted from macOS screenshots are Apple artwork too.** Known cases to replace:
     `src/comum/semaforo_mac27.inc` and `src/relogio/mapa_mundi.inc`.
2. **Does its license allow redistribution, and is it GPL-3.0 compatible?** Good sources:
   - the MacTahoe icon theme (GPL-3.0) for `icones/tahoe/`, converted with `tools/icones-mactahoe.ps1`;
   - WhiteSur (GPL-3.0);
   - Natural Earth (public domain) for maps;
   - fonts under SIL OFL;
   - your own drawing code, which is the best option for buttons and glyphs (see `src/comum/semaforo.h`).
3. **Credit it** in a `CREDITOS.txt` next to the files and in `CREDITS.md`: author, URL, license, and the commit
   or version used.
4. **Apple's assets reach users only by download on their own PC** (`tools/baixar-apple.ps1`, offered by
   `instalador/instalar.ps1`). Never commit what that script downloads, and never bundle it.

The packaging script ships only git-tracked files from `icones/` and refuses fonts, sounds, cursors and videos
(`tools/empacotar.ps1`). That check only sees *which* files are tracked, not *what is inside them*. Replacing a
tracked MacTahoe PNG with an Apple icon gets past it, so look at what a change puts inside tracked files.

## 3. Personal data

- No account names or home paths (`C:\Users\<name>`). Use `%USERPROFILE%` / `$env:USERPROFILE`.
- No logs, registry exports, machine backups, API keys or screenshots of a real desktop. Machine-specific files
  belong in the git-ignored paths in `.gitignore`.

## 4. Notices and branding

- Keep `LICENSE`, the WacOS credit in `README.md`, `CREDITS.md` and `PROVENANCE.md`. A modified third-party file
  keeps its original header and adds a "modified by" notice. See the top of
  `docs/windhawk/bloqueio-sequoia/sequoia-lockscreen.wh.cpp`.
- Don't use the WacOS name or logo, or Apple's logos, as Liquinix branding.

## 5. Before a commit or pull request

Run these and read the output:

```sh
git status --short                                   # nothing untracked under icones/, no FEATURES_TODO.md staged
git diff --cached --name-only | grep -iE '\.(ttf|otf|ttc|cur|ani|wav|aiff?|caf|mp3|m4a|mp4|mov|heic|icns)$'
git diff --cached | grep -nE 'C:\\\\?Users\\\\?[A-Za-z]' | grep -viE 'USERPROFILE|Public'
git diff --cached | grep -n 'renzyxx/WacOS' | grep -v PROVENANCE
```

The second, third and fourth commands should print nothing. If any check fails, stop and tell the user which
file and which rule, instead of working around it. When unsure whether something is allowed, say so and ask.
Guessing "probably fine" is how unlicensed files reach releases.
