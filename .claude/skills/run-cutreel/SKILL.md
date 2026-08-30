---
name: run-cutreel
description: Launch and drive zaro-preview, the CutReel editor, or render one panel to a PNG to look at it. Use whenever asked to run, start, screenshot, or visually check the app.
---

# Running CutReel

`zaro-preview` is a Qt6 desktop GUI. Two ways to see it; pick by what you need.

## Build first

```bash
cmake --build build/debug
```

Presets: `debug`, `release`, `asan` (configure with `cmake -S . -B build/debug --preset debug`).
Warnings are errors, so a build that prints nothing is a build that passed.

## Look at one panel — no permissions needed

**This is the fast path, and the one to reach for by default.** It renders a
widget straight to a PNG under `QT_QPA_PLATFORM=offscreen`, so you can look at
your own change without desktop access, a window, or a real GPU.

`shot.cpp` in this directory is a working harness for `TimelineWidget`. Copy it
to your scratchpad, edit the widget/fixture to suit, then:

```bash
cp .claude/skills/run-cutreel/shot.cpp "$SCRATCH/shot.cpp"
cat >> app/CMakeLists.txt <<'EOF'

# TEMPORARY-SHOT-TARGET
add_executable(zaro_shot ${ZARO_SHOT_SRC})
target_link_libraries(zaro_shot PRIVATE zaro_app_ui Qt6::Widgets)
target_include_directories(zaro_shot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/core/tests)
target_compile_definitions(zaro_shot PRIVATE ZARO_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata/media")
set_target_properties(zaro_shot PROPERTIES AUTOMOC ON)
EOF
cmake -S . -B build/debug -DZARO_SHOT_SRC="$SCRATCH/shot.cpp" >/dev/null
cmake --build build/debug --target zaro_shot
QT_QPA_PLATFORM=offscreen ./build/debug/bin/zaro_shot "$SCRATCH/after.png"
```

Then **read the PNG**. When done: `git checkout app/CMakeLists.txt` and
`cmake -S . -B build/debug -UZARO_SHOT_SRC` — the target must not be committed.

Notes that cost time to discover:

- `core/tests/ModelFixtures.h` (`zaro::testing::Fixture`) builds a project with
  tracks, media and a command stack in one line. Include dir is already wired
  above.
- Render at 2x into a `QPixmap` with `setDevicePixelRatio(2.0)` or text is too
  small to judge.
- Anything that decodes (filmstrips, waveforms) is **asynchronous**. Paint once
  to queue the work, sleep and `processEvents()` in a loop, then paint again for
  the real shot. The harness does this.

## Launch the real app

```bash
./build/debug/bin/zaro-preview.app/Contents/MacOS/zaro-preview <project.zaro> --quiet
```

- **The binary is inside the `.app` bundle.** `build/debug/bin/zaro-preview` is a
  stale leftover that will not have your changes — using it is the easiest way to
  waste ten minutes here.
- A project path is **required**. With no arguments it opens a file dialog and
  blocks.
- `--quiet` puts errors on stderr instead of in modal dialogs. Always pass it
  when launching from a script.

Make a project from the test media:

```bash
./build/debug/bin/zaro-cut "$SCRATCH/demo.zaro" \
  testdata/media/shaky_texture.mov testdata/media/ladder_prores.mov
```

Other flags: `--selftest` (render, verify a picture came out, exit),
`--capture <png>` (with `--selftest`, save what the **program monitor** showed —
not the timeline), `--selftest-quit`, `--locking`.

### Seeing the window

Screenshotting the live GUI needs macOS **Accessibility** and **Screen
Recording** granted to the Claude desktop app. As of the last attempt these were
not granted and `request_access` failed. If you need the real window, ask the
user to grant them first — don't burn a turn retrying. For almost every visual
question the offscreen harness above is faster anyway.

## Test media

Use `shaky_texture.mov`, `wide_texture.mp4` or `ladder_prores.mov` for anything
visual — they have actual texture. `sync_click_flash.mov` is mostly black frames
and makes a working filmstrip look broken.

## Tests

```bash
ctest --test-dir build/debug --output-on-failure
```

794 tests, ~110s. The `zaro_app_tests` target drives a real window and needs a
real platform plugin — it will not run under `QT_QPA_PLATFORM=offscreen`.
