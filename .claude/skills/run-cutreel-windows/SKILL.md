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

### Delete the autosave before an automated run

The app writes `<project>.zaro.autosave` on every close, so the *next* launch
opens a modal **Recover** dialog ("There is a more recent recovery file…").
This is normal, not a crash artefact. It is also a silent trap for scripted
runs: the dialog takes the focus, so clicks and keystrokes aimed at the main
window vanish and playback simply never starts.

```bash
rm -f "$SCRATCH/demo.zaro.autosave"   # before launching
```

A window capture that comes back ~516x194 instead of full size *is* that
dialog — screenshot before assuming a click landed.

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

### A black program monitor almost always means the media did not resolve

**Run the app from the repository root.** `.zaro` projects store media paths
*relative to the working directory*, so launching from `build/windows-release/bin`
resolves nothing: the monitor is black, the audio is silent, and neither the
app nor `--selftest` says why.

```bash
# right
./build/windows-release/bin/zaro-preview.exe "$SCRATCH/demo.zaro" --quiet
# wrong -- no media resolves, and nothing tells you
cd build/windows-release/bin && ./zaro-preview.exe "$SCRATCH/demo.zaro" --quiet
```

`--selftest` reports the symptom, not the cause:

```
grabbed 1268x418, 0.0% of it lit
FAIL: the monitor is essentially black
```

This cost a long detour on 2026-09-02. The trap is that stashing and
re-running "confirms" it is pre-existing — both runs have the same wrong
working directory, so the control varies the code while holding the real cause
fixed. **Before concluding anything from a black monitor, check the media
resolved at all**: run from the repo root, or watch stderr for
`not found: opening testdata/...`.

Anything that decodes fails the same silent way. `AudioGraph` in particular
treats a clip it cannot read as silence rather than an error, so a project
whose media is missing plays perfectly timed nothing.

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

`zaro_core_tests` is deterministic: 790/790 green on `dev` at 4cf0e1e, and any
failure there is real.

**`zaro_app_tests` is non-deterministic, and three runs is not enough.**
Measured repeatedly on 2026-09-02: the failure count sits at **2 or 3**, and
the variation is *not* ordering — two runs with `--order decl`, the same fixed
order, gave 2 and 3. The intermittent case is "Multi-selection, driven as a
rubber band", which passes **8/8 when run alone** and only wobbles inside the
full suite. Mask tracking and the cut-snap case fail nearly always.

A three-run sample happened to give 2/2/2 twice in that session and was twice
treated as ground truth; both times it was luck. At a ~1-in-3 rate three
samples cannot tell "always 2" from "usually 2".

**The cheap decisive test is a filter, not a rebuild.** To find out whether a
test *you* added is responsible, run the same binary with it excluded:

```bash
build/windows-release/bin/zaro_app_tests.exe "~Your test name*"
```

Same binary, same suite, one variable. That settled in four runs what a
stash-rebuild-compare cycle had failed to settle in three attempts.

For a real regression hunt, use six or more runs a side and compare
distributions rather than counts. Every app-test failure reports through
`GuiFixture.cpp(175)`, so grep the message, not the line — and note that a
grep like `^[A-Z][a-z]` silently misses "A cut snaps…".
