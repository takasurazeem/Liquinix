---
name: liquinix-performance
description: Performance and smoothness rules for Liquinix. Use when adding or changing animations, timers, polling, background threads, event hooks, menus, glass/blur, thumbnails, the Spotlight index, or anything that runs while the desktop is idle, and when investigating CPU use, frame rate, stutter, jank, input lag, slow menus or slow app startup. Use it for any change to src/shell, src/finder or the Composition code, even if the request doesn't mention performance.
---

# Liquinix performance rules

Liquinix replaces the desktop the user stares at all day. It is judged on two numbers: whether animations keep
up with the display (tested on a 174 Hz screen), and whether it costs nothing while idle (about 1-2% of one core
with the shell running). Every rule below was measured. The original notes are in `docs/BRIEFING.md` section 9.

## 1. Animate on the display's clock, never with SetTimer

`SetTimer` ticks at about 15.6 ms (64 Hz), so on a high-refresh display it repeats frames and stutters. Use one
of these:

- **In the shell:** `ui::FrameClock(hwnd, msg, on)` and `ui::FrameAck(hwnd, msg)` (`src/shell/shell.h:98`). The
  clock thread waits on `DwmFlush`; call `FrameAck` at the start of each frame handler so frames never queue up.
- **In the Finder and Direct2D apps:** advance the animation in `WM_PAINT`, `Present(1, 0)`, and `Invalidate`
  again only while something is moving.
- **Composition animations** (DirectComposition / Windows.UI.Composition) run in DWM and cost the app nothing per
  frame. Prefer them for movement of whole surfaces (genie, Mission Control, Launchpad).

Step by real time, not by tick count: `anim::Now()` (milliseconds from `QueryPerformanceCounter`,
`src/shell/anim.cpp`). "Move 10% per tick" runs at different speeds on different displays.

Existing `SetTimer` animation code is technical debt, not a pattern to copy. Check before adding to it:
`git grep -n SetTimer src/`.

## 2. Keep the UI thread free

No disk, registry, process, network or `WaitFor*` calls on a thread that owns windows.
- Load shell extensions, thumbnails, the Spotlight index, icons and search on worker threads.
- Thumbnails use 2-4 threads, newest request first.
- Hand results back with `PostMessage`.

A single slow disk read on the UI thread freezes the menu bar and trips the shell's watchdog.

## 3. Preload what the first click needs

A cold context menu took 500-750 ms; preloaded, about 70 ms. The shell warms menus 3 s after start and the Finder
300 ms after start (`ctxmenu::WarmupAsync`), including the menu's glass (D3D/Composition). New first-click
surfaces should do the same.

## 4. Never block on an exit animation

Fades run on their own (`FadeOutAsync`). The chosen action happens immediately, and clicks pass through a menu
that is fading out.

## 5. Idle means idle

- Timers exist only while work is pending. Start them when work arrives and kill them when it's done.
- Prefer events to polling: `SetWinEventHook` with `WINEVENT_OUTOFCONTEXT`, `RegNotifyChangeKeyValue`, shell
  notifications, `WM_SETTINGCHANGE`.
- If polling is unavoidable, poll slowly when nothing is near. For example, hot corners poll slowly while the
  pointer is far from a corner.
- Measured: removing needless polling took the idle shell from about 6% to 2% of a core.

## 6. Measure before and after

Performance claims need numbers. The logs in `bin\*.log` already print:
- `menu na tela em X ms`, `QueryContextMenu: X ms`
- `abertura (reserva): ... X ms`
- `pasta: ... (N itens, X ms)`
- `rolagem: N quadros em X ms (F quadros/s)`
- `vidro real: 10 s: ... quadros ... montagens`

Add a similar `Log(L"...")` line for anything new you optimise. Report CPU idle and active separately: idle is
nothing touched for 30 s; active is during the interaction.

If you are working from Linux and cannot run the code, say the numbers were not measured. Don't estimate them.

## 7. Linux port

The same rules apply to the Linux backend:
- frame callbacks (`wl_surface.frame`, or the toolkit's frame clock), never fixed-interval timers
- no blocking I/O on the main loop
- event sources (D-Bus signals, inotify) instead of polling
