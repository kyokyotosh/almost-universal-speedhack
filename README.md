# almost-universal-speedhack

A speedhack for single-player Windows games, with a Qt front end.

It injects a small DLL into a target process and hooks the standard Windows
timing APIs, returning a scaled value so the game believes more (or less) time
has passed than really has. Most games drive their update loop off these APIs,
so scaling them speeds up or slows down the whole game. You pick a process,
inject, and drive the multiplier live from a slider.

The technique is the same one Cheat Engine's speedhack uses; this is a C++
reimplementation built mostly as a way to learn Win32 hooking, IPC, and Qt.

## Why "almost" universal

There is no such thing as a truly universal speedhack without a kernel driver.
Some games read the CPU timestamp counter directly (`rdtsc`) instead of calling
a hookable API, and those bypass this entirely — trapping `rdtsc` needs ring 0.
What you get here covers games that use the normal Win32 timing functions, which
is most of them, but not all. See Limitations below.

## How it works

The injected DLL hooks four timing functions with [MinHook](https://github.com/TsudaKageyu/minhook):

- `GetTickCount`
- `GetTickCount64`
- `timeGetTime`
- `QueryPerformanceCounter`

Each hook keeps an anchor: the real and fake clock values at the moment the
speed last changed. New readings are computed as
`fake_anchor + (now_real - real_anchor) * multiplier`. Anchoring on every change
keeps the fake clock continuous, so toggling speed mid-game never makes time jump
backward and break physics.

Speed is controlled through a named shared-memory section (`Local\Speedhack_<pid>`).
The GUI writes a multiplier and bumps a version counter; a watcher thread in the
DLL picks up the change and re-anchors. The control struct has a fixed 16-byte
layout, so the same section works across the x86/x64 boundary unchanged.

## Building

Requirements: CMake 3.16+, a C++17 compiler, and Qt 6 (developed against 6.11,
MinGW). MinHook is pulled in automatically via FetchContent.

```
cmake -B build
cmake --build build
```

This produces three artifacts in `build/bin`:

- `almost-universal-speedhack` — the Qt GUI
- `speedhack.dll` — the injected payload
- `harness` — a console test program that loads the DLL into itself and prints
  the timing output, useful for verifying the hooks without a game

Layout:

```
shared/   speedhack_ipc.h   (the IPC contract, shared by all three)
gui/      Qt front end + the injector
dll/      the payload
harness/  standalone test
```

## Usage

Run the GUI. Hit Refresh, select the target from the list, and click Inject.
Tick "Enable speedhack" and move the slider. The slider is logarithmic from
0.1x to 500x; type an exact value in the box next to it if you want one.

Hotkeys work while the game has focus:

- `Ctrl+Alt+Home` — toggle on/off
- `Ctrl+Alt+End` — reset to 1x
- `Ctrl+Alt+PageDown` — cycle presets

The injector and the target must be the same architecture. The build is
currently x64, so it injects x64 games; a 32-bit target shows a bitness warning.
An x86 build path is in progress.

## Limitations

- Single-player only. The hooks are trivially detectable by comparing a hooked
  timer against an unhooked one, so this will trip anti-cheat. Do not use it
  online.
- Games that read `rdtsc` directly are not affected.
- Fixed-timestep engines may not scale cleanly. Pushed too far, they spend the
  frame catching up and the framerate collapses instead of speeding up. Idle and
  accumulator games tend to tolerate very high multipliers; simulation-heavy
  games much less.
- Browser and Electron games run their logic in a sandboxed renderer that does
  not route through these APIs. This approach does not work on them.
- Offline/away progress in idle games is usually computed from the real calendar
  clock (`GetSystemTimeAsFileTime`), which is deliberately not hooked. Active
  play speeds up; "you were gone 10 hours" does not.
