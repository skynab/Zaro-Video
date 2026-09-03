---
name: run-cutreel-windows
description: Run, screenshot or visually check CutReel (zaro-preview) ON WINDOWS. Covers priming the MSVC build shell, the build/windows-release tree, launching the .exe, and capturing the live window with PowerShell. Use only on Windows -- on macOS use run-cutreel-macos instead, whose .app bundle path and offscreen harness do not work here.
---

# Running CutReel on Windows

**Platform: Windows only.** On macOS use the `run-cutreel-macos` skill. The two
differ in more than paths: the offscreen harness that skill leads with **does
not work here at all**, and the binary is a plain `.exe` rather than an `.app`
bundle.

## Prime the build shell first

Nothing below is on `PATH` by default, and the Bash tool's environment does not
persist between calls. Write a `.bat` wrapper into your scratchpad once and
invoke it with `cmd //c`:

```bat
@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "PATH=%USERPROFILE%\devtools\cmake-3.31.6-windows-x86_64\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
set "VCPKG_INSTALLATION_ROOT=%USERPROFILE%\devtools\vcpkg"
cd /d "%~dp0..\..\.."
cmake --build build\windows-release --target %1 2>&1
```

Those tool locations are how this machine happens to be set up rather than
anything the project mandates; if a path is wrong, find the real one rather
than assuming the tool is missing.

`vcvars64.bat` prints a harmless `'vswhere.exe' is not recognized` line; ignore
it. Use CMake 3.31.6 from `devtools`, **not** CMake 4.x — 4.x drops
`cmake_minimum_required` < 3.5, which the FetchContent'd Catch2 build trips on.

The build tree is `build/windows-release` (RelWithDebInfo). There is no
`build/debug` on this box. Warnings are errors, so a quiet build passed.

Useful targets: `zaro-preview`, `zaro_core_tests`, `zaro_app_tests`.

### LNK1168: close the app first

```
LINK : fatal error LNK1168: cannot open bin\zaro-preview.exe for writing
```

means a `zaro-preview.exe` is still running and Windows has the file locked.
Check with `tasklist //FI "IMAGENAME eq zaro-preview.exe"`. **Ask before killing
it** — it may be the user's own session with unsaved work. Every static library
and both test binaries still build while it is locked, which is enough to
confirm a change compiles and links; only the final exe link is blocked.

## Launch the app

```bash
cd build/windows-release/bin && cmd //c start "" zaro-preview.exe <project.zaro> --quiet
```

- The binary is `build/windows-release/bin/zaro-preview.exe`. There is no
  `.app` bundle on Windows — that is the macOS skill.
- `start ""` detaches, so the Bash call returns instead of blocking the turn.
- A project path is optional; with none it opens an untitled empty project.
  Under `--quiet` a *missing* project is an error (exit 2) rather than a modal.
- `--quiet` puts errors on stderr instead of in dialogs. Always pass it.

Make a project from the test media:

```bash
./build/windows-release/bin/zaro-cut.exe "$SCRATCH/demo.zaro" \
  testdata/media/shaky_texture.mov testdata/media/wide_texture.mp4
```

Other flags: `--selftest` (render, verify a picture came out, exit),
`--capture <png>`, `--selftest-quit`, `--locking`.

## Screenshotting the live window

**`QT_QPA_PLATFORM=offscreen` does not work here.** Only
`platforms/qwindows.dll` is staged in `build/windows-release/bin`, so with that
variable set `zaro-preview` and any `zaro_shot` harness die silently — no
output, no exit message. Drop the variable. If you want the macOS skill's
one-panel harness, render to a `QPixmap` from a widget that was never shown;
that works under the normal windows plugin.

To capture the real window, use `shotwin.ps1` in this directory — PowerShell +
`System.Drawing`, `GetWindowRect` on the process's `MainWindowHandle`, then
`Graphics.CopyFromScreen`:

```bash
powershell -ExecutionPolicy Bypass -File .claude/skills/run-cutreel-windows/shotwin.ps1 -Out "$SCRATCH/live.png"
```

Then **read the PNG**. Clicks can be driven the same way with `SetCursorPos` +
`mouse_event`, and native file dialogs with `[System.Windows.Forms.SendKeys]`.

### The program monitor captures black — this is not your bug

The program monitor is a `QRhiWidget` on a D3D swapchain, and that surface is
**not** in either capture route: not `QWidget::grab`/`grabFramebuffer`, and not
`CopyFromScreen` off the desktop. So the big video pane reads as solid black in
every screenshot while the real window shows picture fine.

`--selftest` reports this as a failure:

```
grabbed 1268x418, 0.0% of it lit
FAIL: the monitor is essentially black
```

Verified on 2026-09-02 to be identical on an unmodified `dev`, so it says
nothing about your change. **Before believing a black monitor is a regression,
stash and run the same capture on the baseline.** Everything else in the window
— timeline, media pane, inspector, timecode — captures correctly and is what
these screenshots are actually good for.

`zaro-preview --selftest --capture <png>` writes two files: the monitor's
`grabFramebuffer` at `<png>` and the whole window at `<png>.window.png`.

## Test media traps

Use `shaky_texture.mov` or `wide_texture.mp4` for anything visual — they have
real texture. Two fixtures look like failures and are not:

- `sync_click_flash.mov` is black except on flash frames (it is the one with an
  audio stream, so it is still the right pick for exercising playback sound).
- `ladder_prores.mov` frames are flat grey steps.

## Tests

```bash
build/windows-release/bin/zaro_core_tests.exe
build/windows-release/bin/zaro_app_tests.exe
```

`zaro_app_tests.exe` drives a real window: it needs `~/devtools/Qt/6.9.3/msvc2022_64/bin`
on `PATH` and a real platform plugin (not offscreen).

Baseline on 2026-09-02 (`dev` at 4cf0e1e): core 790/790 green; app tests 84
cases, 83 pass, **one pre-existing failure** — "tracking the mask showed no more
of the picture than leaving it behind did". Anything else is yours. The `[gui]`
cases are GPU-dependent and the failing set has drifted before, so re-measure
by stashing rather than trusting that number. Every app-test failure reports
through `GuiFixture.cpp(175)`, so grep the message, not the line.
