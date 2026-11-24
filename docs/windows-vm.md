# Testing Liquinix in a Windows VM

How to build, run and check Liquinix's Windows code from a Linux computer, in a Windows 11 virtual machine. CI
(`.github/workflows/build.yml`) tells you whether the code compiles; the VM is where you see whether it works.
For the Linux side of the project, see `docs/linux-dev.md`.

Two ways to set it up, both with KVM/QEMU on Fedora-based systems (Nobara):
- **[quickemu](#quick-route-quickemu)**: tested end to end (November 2026). It needs no root, is scriptable,
  and is what an AI agent can drive over SSH.
- **virt-manager** (sections 1 to 6): the usual graphical route. It hasn't been run end to end yet, so fix
  this file when something differs.

Sections 5 to 7 (building, snapshots, the visual check) apply to both.

## What the VM is for

| Good for | Not good for |
|---|---|
| Does it work: menus, Dock, Finder, Settings, the apps | Performance: idle CPU, frame rate, stutter |
| How it looks: light, dark, inactive windows, themes | Glass, blur and animation quality |
| Title bars injected into other apps (`build.cmd janelas`) | Anything that depends on a specific real GPU |
| The installer, updater and uninstaller | |
| Replacing Explorer as the shell (`docs/sem-explorer.md`) | |
| Translations and HiDPI (100%, 150%, 200%) | |

Without a passed-through GPU, Windows in a VM renders everything in software (WARP). Direct2D and
DirectComposition still work, but slowly, and some glass and blur effects may look different or be turned off.
Do performance work and final checks before a release on a real Windows PC.

Replacing the Windows shell (`Winlogon\Shell`) is only ever tested in the VM, never on a real PC (see the
`liquinix-build-verify` skill).

## Quick route: quickemu

[quickemu](https://github.com/quickemu-project/quickemu) wraps QEMU with a config file per VM: UEFI, an emulated
TPM 2.0, an unattended Windows install with a local account, and a SPICE window with clipboard sharing.

```sh
sudo dnf install quickemu edk2-ovmf   # quickemu brings QEMU, swtpm (TPM) and spicy; edk2-ovmf is the UEFI firmware
mkdir -p ~/VMs && cd ~/VMs
quickget windows 11              # writes windows-11.conf and downloads the virtio drivers and the answer file
```

What we ran into, and the fixes:

- **Microsoft may block the ISO download** ("blocked the automated download request based on your IP"). Download
  a Windows 11 ISO in a browser instead. The **Windows 11 Enterprise evaluation** from the
  [Evaluation Center](https://www.microsoft.com/evalcenter/download-windows-11-enterprise) works without a key
  for 90 days, which is fine for a test VM. Save it as `~/VMs/windows-11/windows-11.iso`.
- **The evaluation ISO rejects quickget's Pro key.** quickget's `windows-11/unattended/autounattend.xml` sets
  Microsoft's public setup key for Windows Pro. For the Enterprise evaluation, remove both `<ProductKey>`
  entries and rebuild the answer disc:
  `mkisofs -quiet -J -o windows-11/unattended.iso windows-11/unattended/` (keep backups outside that folder, or
  they end up on the disc).
- **The virtio driver download can come back as a 4 KB HTML page.** fedorapeople.org shows a bot check to
  quickget's browser user agent. A plain `curl` gets the real file:
  `curl -fL -o windows-11/virtio-win.iso https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/virtio-win.iso`
  (check that it's several hundred MB). Windows Setup needs it: quickemu gives Windows a virtio disk.
- **Clipboard sharing needs the SPICE display.** Add these to `windows-11.conf`:

  ```
  display="spice"
  ram="8G"
  cpu_cores="4"
  disk_size="80G"
  ```

Then `quickemu --vm windows-11.conf`. Setup asks for the language and keyboard (press Enter on both) and does
the rest, including a local account `Quickemu` (password `quickemu`). It's a neutral name, so screenshots can
go into pull requests. In Windows:

- **Check the drivers:** `Get-PnpDevice | Where-Object { $_.Status -ne 'OK' -and $_.Present }` should print
  nothing. If it doesn't, run `virtio-win-guest-tools.exe` from the virtio-win CD.
- **Install Git and Build Tools.** Older winget versions fail on the Microsoft Store source with a certificate
  error, so name the source:
  `winget install --id Git.Git -e --source winget` and
  `winget install --id Microsoft.VisualStudio.2022.BuildTools -e --source winget --override "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`.
- **Optional, for driving it from Linux:** turn on OpenSSH Server (Settings > System > Optional features, or
  `Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0`, then `Start-Service sshd`). quickemu
  forwards host port 22220 to it: `ssh -p 22220 Quickemu@localhost`. Programs started over SSH run in an
  invisible session; to show one on the desktop, start it with a scheduled task that uses
  `New-ScheduledTaskPrincipal -LogonType Interactive`.
- **Turn off the screen timeout** (`powercfg /change monitor-timeout-ac 0`). If the display is off when the
  shell starts, Liquinix's own lock screen comes up (`bloqueio_ao_apagar`).

Snapshots, with the VM shut down:

```sh
quickemu --vm windows-11.conf --snapshot create clean
quickemu --vm windows-11.conf --snapshot apply clean     # go back to it
```

The QEMU monitor (`windows-11/windows-11-monitor.socket`) can take screenshots (`screendump file.ppm`) and type
(`sendkey ret`). With the SPICE display, screenshots work once Windows has loaded the QXL driver.

## 1. Install the virtualisation tools

```sh
sudo dnf install @virtualization        # virt-manager, libvirt, QEMU/KVM, swtpm (TPM), edk2-ovmf (UEFI)
sudo usermod -aG libvirt "$USER"         # manage VMs without a password prompt; log out and back in
```

Check that hardware virtualisation is on: `grep -cE 'vmx|svm' /proc/cpuinfo` should print a number above 0. If it
prints 0, turn on Intel VT-x or AMD-V (SVM) in the firmware settings.

## 2. Download Windows 11 and the drivers

- **Windows 11 ISO:** from Microsoft's download page (microsoft.com/software-download/windows11), "Download
  Windows 11 Disk Image (ISO) for x64 devices". English (United States) or Portuguese (Brazil), depending on
  which language you want to test first; the other can be added later as a display language.
- **virtio-win drivers:** `virtio-win.iso` from the Fedora project
  (fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/). It has the display, network and
  disk drivers and the guest tools (clipboard sharing, resizing the window).

Windows runs without a product key. Unactivated, it shows a watermark and locks some personalisation settings
(wallpaper, accent colour). Liquinix's own settings aren't affected; anything that needs Windows' personalisation
settings needs an activated copy.

## 3. Create the VM

In virt-manager: **File > New Virtual Machine > Local install media**, pick the Windows ISO, and let it detect
"Microsoft Windows 11". With that OS chosen, virt-manager adds UEFI with Secure Boot and an emulated TPM 2.0,
which Windows 11 requires.

- **Memory and CPUs:** 8 GB and 4 CPUs is comfortable; 4 GB and 2 CPUs works, but building is slow.
- **Disk:** 80 GB. Build Tools 2022 take about 8 GB.
- **Name:** `liquinix-win11` (the snapshot commands below use it).
- Tick **Customize configuration before install**, then **Add Hardware > Storage > CDROM device** and choose
  `virtio-win.iso`, so the drivers are at hand during setup.

Keep the default SATA disk and e1000e network card. They work without extra drivers, so setup doesn't get stuck
looking for the disk.

## 4. Set up Windows

- **Use a local test account, not your Microsoft account.** Name it something neutral like `tester`. Screenshots
  and logs from the VM can then go into issues and pull requests without personal data (AGENTS.md rule 4).
  Windows setup pushes for a Microsoft account; how to skip it changes between Windows builds, so look up the
  current way for the build you downloaded (usually involving Shift+F10 during setup).
- **Install the guest tools:** open the virtio-win CD in the VM and run `virtio-win-guest-tools.exe`. Afterwards
  the VM window can be resized, and copy and paste works between Linux and Windows.
- **Install Git and Build Tools 2022** (PowerShell):

  ```powershell
  winget install --id Git.Git -e
  winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  ```

- **Clone the repository:** `git clone https://github.com/takasurazeem/Liquinix.git` in
  `%USERPROFILE%\Developer`.
- **Shut down and take the first snapshot** (section 6), called `clean`. Everything after this point can be
  undone by reverting to it.

## 5. Build and run a change

Push your branch from Linux, then in the VM:

```powershell
cd $env:USERPROFILE\Developer\Liquinix
git fetch
git switch <branch>      # or: git pull, on a branch you already have
.\build.cmd              # add a target to build just one part, e.g. .\build.cmd janelas (list in build.cmd)
```

Then follow "On Windows" in `.claude/skills/liquinix-build-verify/SKILL.md`: free the locked executables, build,
and restart the shell.

To test the installer the way users get it, package the build and install from the zip:

```powershell
powershell -ExecutionPolicy Bypass -File tools\empacotar.ps1 -SemSistema -SemAjustes
# extract dist\Liquinix-<date>.zip somewhere and open LiquinixSetup.exe
```

**Ways out when something breaks:**
- **Quit Liquinix:** Ctrl+Alt+Shift+F12.
- **Emergency** (closes the shell and brings Explorer back): Ctrl+Alt+Shift+F11.
- **Still stuck:** force off the VM and revert to the last snapshot.

The VM viewer may catch some key combinations before Windows sees them (virt-manager uses Ctrl+Alt to release
the mouse and keyboard). If a Liquinix shortcut doesn't reach Windows, click inside the VM first so it grabs the
keyboard. For Ctrl+Alt+Del, use **Send Key** in the viewer's menu.

## 6. Snapshots

Take one before every install, shell replacement or registry change, and revert instead of uninstalling.

virt-manager's snapshot button works on recent versions, but older ones refuse VMs that use UEFI. Snapshots of the
disk with `qemu-img` always work. Run them with the VM shut down:

```sh
sudo qemu-img snapshot -c clean /var/lib/libvirt/images/liquinix-win11.qcow2    # create
sudo qemu-img snapshot -l /var/lib/libvirt/images/liquinix-win11.qcow2          # list
sudo qemu-img snapshot -a clean /var/lib/libvirt/images/liquinix-win11.qcow2    # revert
sudo qemu-img snapshot -d old /var/lib/libvirt/images/liquinix-win11.qcow2      # delete
```

These snapshot the disk only, not the UEFI or TPM state. That's fine as long as you don't change Secure Boot or
turn on BitLocker in the VM.

## 7. A visual check

For a change to drawing, title bars or themes (card #5's window buttons, for example), check each of:
- light and dark mode, and an active and an inactive window;
- hover and pressed;
- Finder, System Settings, and a regular Windows app with injected title bars (Notepad);
- 100% and 200% scaling (Windows Settings > System > Display > Scale; give the VM a large enough resolution).

Attach screenshots from the VM to the pull request, and say in it what was checked in the VM and what still
needs a real PC.

## Other notes

- **Getting unpushed code into the VM:** pushing a work-in-progress branch is the simplest way. A shared folder
  (virtiofs) also works, but needs WinFsp and the virtio-fs service set up in Windows.
- **Using CI's build:** later, the build workflow can upload `bin\` as an artifact, so the VM can test exactly
  what CI built without Build Tools installed. It doesn't do that yet.
- **`tools/vm-*.ps1`** are upstream scripts for a Hyper-V VM run from a Windows host (the `AuthUX-Teste` VM).
  They don't apply to a Linux host.
