# Roadmap: pick order

This is the order in which to work on the cards of the
[Liquinix project board](https://github.com/users/takasurazeem/projects/10). Each card is a GitHub issue. The
issue says what to do and when it's done; this file says **which one is next**.

**Where the truth lives:**
- **Order** is in this file.
- **Status** is on GitHub: an issue is done when it is closed. Don't track status here; it would drift.
- **Details** are in each issue's body.

## Picking the next card

At the start of a session, or after finishing a card:

1. **List the open issues:**
   `gh issue list -R takasurazeem/Liquinix --state open --limit 100 --json number,title`
2. **Walk the table below from the top.** Pick the first issue that is still open **and** whose
   "Needs" issues are all closed. Skip one only if it needs something the session can't do, such as a Windows
   machine when you're working from Linux, and say why you skipped it.
3. **Read the issue:** `gh issue view <N> -R takasurazeem/Liquinix`. Confirm the choice with the user before
   starting.
4. **Mark it In Progress** on the board, if you can. Work on a new branch from `main`, one card per branch.
5. **Open a pull request** with `Fixes #N` in the description, so merging closes the issue and moves the card to
   Done. Follow `AGENTS.md` and the matching skill in `.claude/skills/`.

If you find new work, open an issue, add it to the board, and add it to this table in the same pull request. Put
it where it belongs, not just at the end.

The board has an **Order** field with the same numbers as the table below, so it can be sorted to match. When
the order changes, update both.

## Order

| Order | Issue | Card | Priority | Needs | Can run on |
|---:|---|---|---|---|---|
| 1 | [#5](https://github.com/takasurazeem/Liquinix/issues/5) | Replace the Apple-derived window buttons in `semaforo_mac27.inc` | P0 | none | Windows to verify (code can be written anywhere) |
| 2 | [#6](https://github.com/takasurazeem/Liquinix/issues/6) | Rebuild the Clock world map from Natural Earth data | P0 | none | Any (generator is Python; verify on Windows) |
| 3 | [#17](https://github.com/takasurazeem/Liquinix/issues/17) | Linux: decide the toolkit and first target desktop | P1 | none | Any (a decision with the user, written to `docs/linux-roadmap.md`) |
| 4 | [#7](https://github.com/takasurazeem/Liquinix/issues/7) | Credit Open-Meteo in Weather, Clock and the desktop widget | P1 | none | Windows to verify |
| 5 | [#8](https://github.com/takasurazeem/Liquinix/issues/8) | Load `MacDialogo.dll` and `LiquinixWindow.dll` by absolute path | P1 | none | Windows to verify |
| 6 | [#9](https://github.com/takasurazeem/Liquinix/issues/9) | Test the release package on Windows and publish the first release | P1 | #5, #6 | Windows only |
| 7 | [#19](https://github.com/takasurazeem/Liquinix/issues/19) | Extract a platform layer on Windows (no behaviour change) | P1 | #17 | Windows to verify (CI builds it) |
| 8 | [#18](https://github.com/takasurazeem/Liquinix/issues/18) | Linux: toolbox and CMake build skeleton | P1 | #17 | Linux |
| 9 | [#20](https://github.com/takasurazeem/Liquinix/issues/20) | Linux: first app, `liquinix-dock` as a layer-shell window | P2 | #18, #19 | Linux |
| 10 | [#21](https://github.com/takasurazeem/Liquinix/issues/21) | Linux CI job with headless KWin | P2 | #18 | Any |
| 11 | [#12](https://github.com/takasurazeem/Liquinix/issues/12) | Rewrite the README for Windows and Linux | P2 | none | Any |
| 12 | [#10](https://github.com/takasurazeem/Liquinix/issues/10) | Design a Liquinix logo and installer icon | P2 | none | Any (needs the user's design input) |
| 13 | [#11](https://github.com/takasurazeem/Liquinix/issues/11) | New screenshots and a social preview image | P2 | #10 | Windows (and Linux once the dock exists) |
| 14 | [#15](https://github.com/takasurazeem/Liquinix/issues/15) | Harden the GitHub Actions workflow | P3 | none | Any |
| 15 | [#16](https://github.com/takasurazeem/Liquinix/issues/16) | Small licensing cleanups in the Windhawk mod and docs | P3 | none | Any |
| 16 | [#14](https://github.com/takasurazeem/Liquinix/issues/14) | Move the Calculator's key animation off `SetTimer` | P3 | none | Windows to verify |
| 17 | [#33](https://github.com/takasurazeem/Liquinix/issues/33) | Move the remaining `SetTimer` animations onto the display clock | P3 | #14 | Windows to verify |
| 18 | [#13](https://github.com/takasurazeem/Liquinix/issues/13) | Update `docs/BRIEFING.md` for the fork | P3 | none | Any |

**Why this order:**
- **1–6 make a clean first release possible:**
  - The Apple-derived artwork goes first (P0).
  - The Linux toolkit decision comes early because it's a conversation, not code, and it unblocks the Linux
    track.
  - The two small fixes (Open-Meteo credit, DLL paths) come before the release.
  - The release itself waits on the P0s.
- **7–10 start the Linux port** in dependency order.
- **11–18 are polish:** pick them up whenever there's a gap, or when the main track is blocked.

**"Can run on":** most of the code is Windows-only. A session on Linux can write Windows code, but it can't build
or run it; CI builds only `build.cmd` and `build.cmd janelas`. Prefer cards a session can actually verify, and
when it can't, say so in the pull request (see the `liquinix-build-verify` skill).
