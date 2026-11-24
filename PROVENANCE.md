# Provenance

Liquinix is a fork of **WacOS** by renzyxx (<https://github.com/renzyxx/WacOS>), taken from the last revision of
WacOS that was published under the **GNU General Public License v3.0**. This file records where that code came
from and how anyone can check it independently.

## Upstream revision

| | |
|---|---|
| Upstream repository | <https://github.com/renzyxx/WacOS> |
| Forked commit | `0afb96a69ae2fd130621de41a74f1880aa1a9271` ("Merge Dock on the left or right of the screen") |
| Commit date | 2026-11-20 20:11:12 UTC |
| Tree of that commit | `52da3f628de1c84be80530d4c87c5c764bc89439` |
| License in that tree | GNU GPL v3.0 (`LICENSE`, unchanged in this repository) |

## Upstream license history

| Commit | Date (UTC) | License from this commit on |
|---|---|---|
| `2beced5c913f58ae7740408b492933eace017de2` | 2026-11-20 05:29:59 | MIT |
| `1b5a8c8d79cf5848a31204cc2971231dc2e510ac` | 2026-11-20 05:39:39 | GPL-3.0 |
| `0afb96a69ae2fd130621de41a74f1880aa1a9271` | 2026-11-20 20:11:12 | GPL-3.0 (**forked here**) |
| `7c0758d5b7ec08b5a234f86732b3a408bb03ce3e` | 2026-11-20 20:17:43 | Proprietary "source-available" license, "from build-17 on" |

The upstream relicensing commit itself says the new license applies "from build-17 on", and its own section 5
("Earlier versions") acknowledges that earlier snapshots were published under other licenses. A GPL-3.0 grant
for copies already distributed cannot be withdrawn (GPL-3.0, section 2: "All rights granted under this License
are granted for the term of copyright on the Program, and are irrevocable provided the stated conditions are
met.").

Liquinix contains **no code from any upstream commit after `0afb96a6`**. Features that upstream added later are
written independently, from their public descriptions only.

## Independent archives

Copies made by third parties on 2026-11-24, so the upstream history can be checked even if it is changed later:

- Software Heritage, save request 2688360 for `https://github.com/renzyxx/WacOS`, archived 2026-11-24 18:24:55 UTC
  as snapshot `swh:1:snp:4f227263bc5bf4373543f247186fbffd7fd42f31`:
  <https://archive.softwareheritage.org/browse/origin/?origin_url=https://github.com/renzyxx/WacOS>
  (look up the revision `0afb96a69ae2fd130621de41a74f1880aa1a9271`; Software Heritage IDs:
  `swh:1:rev:0afb96a69ae2fd130621de41a74f1880aa1a9271`, `swh:1:dir:52da3f628de1c84be80530d4c87c5c764bc89439`)
- Internet Archive (Wayback Machine):
  - GPL `LICENSE` at the forked commit:
    <https://web.archive.org/web/20261124182457/https://github.com/renzyxx/WacOS/blob/0afb96a69ae2fd130621de41a74f1880aa1a9271/LICENSE>
  - The forked tree:
    <https://web.archive.org/web/20261124182613/https://github.com/renzyxx/WacOS/tree/0afb96a69ae2fd130621de41a74f1880aa1a9271>
  - The switch from MIT to GPL-3.0:
    <https://web.archive.org/web/20261124182516/https://github.com/renzyxx/WacOS/commit/1b5a8c8d79cf5848a31204cc2971231dc2e510ac>
  - The switch to the proprietary license:
    <https://web.archive.org/web/20261124182535/https://github.com/renzyxx/WacOS/commit/7c0758d5b7ec08b5a234f86732b3a408bb03ce3e>
  - LICENSE history on `main`:
    <https://web.archive.org/web/20261124182553/https://github.com/renzyxx/WacOS/commits/main/LICENSE>

Git object IDs are hashes of their contents, so a commit or tree with the IDs above can only have the exact files
recorded here, wherever a copy is found.

## What changed in the import

The first commit of Liquinix is the upstream tree with these changes only:

- **Removed (645 files)**, because they are not covered by the GPL, contain the original author's personal data,
  or carry the WacOS brand:
  third-party icon packs (`icones/sequoia`, `icones/extra`, `icones/tahoe-macosicons`), Apple SF Symbols
  (`icones/simbolos`), Apple wallpapers (`icones/bloqueio`), screenshots (`docs/screenshots`, `docs/authux-*`,
  `docs/vm-*`, `docs/fotos-ilha`, `docs/diagnostico`), machine-specific backups and logs from `docs/`, and the WacOS logo (`docs/logo.png`, `docs/logo-256.png`,
  `tools/logo-wacos.py`).
- **Modified (6 files)**: the original author's Windows account path was replaced with `%USERPROFILE%` /
  `$env:USERPROFILE` (`docs/BRIEFING.md`, `docs/sem-explorer.md`, `src/finder/main.cpp`, in two comments only,
  `tools/criar-vm-authux.ps1`, `tools/vm-authux-pacotes.ps1`), and `.gitignore` gained entries for those backups.

Every other file is byte-for-byte identical to upstream.

## How to verify

```sh
git clone https://github.com/takasurazeem/Liquinix.git && cd Liquinix
git fetch https://github.com/renzyxx/WacOS.git 0afb96a69ae2fd130621de41a74f1880aa1a9271
git rev-parse FETCH_HEAD^{tree}                       # 52da3f628de1c84be80530d4c87c5c764bc89439
git show FETCH_HEAD:LICENSE | head -2                 # GNU GENERAL PUBLIC LICENSE, Version 3
IMPORT=$(git rev-list --max-parents=0 HEAD)
git diff --name-status FETCH_HEAD "$IMPORT" | cut -f1 | sort | uniq -c   # 645 D, 6 M
git diff FETCH_HEAD "$IMPORT" --diff-filter=M          # only the changes listed above
```

If the upstream commit is ever removed from GitHub, fetch it from Software Heritage instead.
