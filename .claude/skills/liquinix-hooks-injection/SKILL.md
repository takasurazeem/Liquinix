---
name: liquinix-hooks-injection
description: Safety rules for Liquinix code that runs inside other programs or intercepts system input. Use when editing src/janelas (LiquinixWindow.dll, 32 and 64-bit, injected into other apps for Mac title bars), src/titulos, src/dialogo (Open/Save dialog DLL), src/shell/janelas.cpp, any SetWindowsHookEx or SetWinEventHook call, low-level keyboard or mouse hooks (Win key, shortcuts, screenshots), LoadLibrary calls, WM_COPYDATA handlers, or the game/anti-cheat exclusion list in src/comum/jogos.h. Also use when another app crashes, hangs, or gets flagged by antivirus because of Liquinix.
---

# Hooks and injected code

Some Liquinix code runs inside other people's programs (Chrome, Discord, OBS, games) or sits in the path of every
keystroke. A bug there doesn't crash Liquinix; it crashes the user's browser, freezes their typing, or gets them
banned by an anti-cheat. Be more careful here than anywhere else.

## Where it lives

- **`LiquinixWindow.dll`** (`src/janelas/macjanela.cpp`) is loaded into other apps through `WH_GETMESSAGE` hooks
  set by the shell (`src/shell/janelas.cpp:190`, `:231`). Inside the app it adds a `WH_CALLWNDPROCRET` hook per
  thread (`macjanela.cpp:1806`, `:1826`, `:1844`).
- **32-bit apps** get the 32-bit DLL through a host in Windows' 32-bit `rundll32.exe` (the `LiquinixHost` entry,
  the `MacJanela32Host` window).
- **Low-level keyboard hooks:** `src/shell/main.cpp:275` (Win key, Mac shortcuts) and `src/captura/main.cpp:1187`
  (screenshots).
- **Low-level mouse hooks:** `src/shell/menus.cpp:456` and `src/shell/revelar.cpp:487`.
- **Exclusions:** `src/comum/jogos.h` decides which processes must never receive the DLL (games, anti-cheat).

## Rules inside another process

- **Never crash or hang the host.** Check every pointer and HWND, and catch failure paths. Never wait on a lock,
  event or cross-process message from inside a hook or the host's window procedure.
- **Do little in hook procedures.** Do the minimum, post a message for the rest, and always call
  `CallNextHookEx`.
- **No CRT or C++ exceptions crossing the DLL boundary.** Don't allocate in a way the host could free, and don't
  assume the host's threading model or DPI awareness.
- **Unhook and clean up on unload,** and on `DLL_PROCESS_DETACH` do only what is safe under the loader lock (no
  waiting, no `LoadLibrary`).
- **Keep 32-bit and 64-bit in sync.** Every change to `src/janelas` builds both (`build.cmd janelas`). Watch
  pointer-size and struct-packing differences in shared messages.

## Low-level input hooks

Windows silently removes a `WH_KEYBOARD_LL` or `WH_MOUSE_LL` hook whose callback is too slow
(`LowLevelHooksTimeout`, about 1 s, less under load). After that the Win key and shortcuts stop working with no
error.
- Return within microseconds: no I/O, no logging to disk, no `SendMessage` to other processes in the callback.
- Post a message and handle the rest elsewhere.
- Don't synthesize input (`SendInput`) on the user's PC unless the feature needs it. It confuses accessibility
  tools and anti-cheats.

## Loading DLLs

- Load by **absolute path** (`LoadLibraryExW` with a full path and `LOAD_WITH_ALTERED_SEARCH_PATH`, as
  `src/comum/barra_mac.h:22` does), or system DLLs with `LOAD_LIBRARY_SEARCH_SYSTEM32` (as `src/relogio/main.cpp`
  does).
- A bare name such as `LoadLibraryW(L"MacDialogo.dll")` (`src/shell/dialogs.cpp`) searches the current directory
  and PATH. A planted DLL there runs with the user's rights. Fix these when you touch them.

## Messages from other processes

Treat `WM_COPYDATA` and registered messages as untrusted input:
- Check `cbData` against the expected size before reading.
- Never trust lengths or offsets inside the payload.
- Ignore messages from senders you don't expect.

## Games and anti-cheat

Injecting into a protected game can get the user banned. Keep `src/comum/jogos.h` conservative: when a new game
or launcher misbehaves, add it to the exclusions rather than working around its protection. Never try to bypass
anti-cheat or code-signing checks.

## Reviewing

For a deep audit of `src/janelas`, the hooks or the message handlers, use `/security-review` on the change, or a
memory-safety review (for example Trail of Bits' `c-review` plugin, if installed). Build both bitnesses, and test
with a few real apps (a Chromium browser, an Electron app, a 32-bit app) before calling it done.
