---
name: clang-format
description: Keep C++ sources clang-format clean so the CI format job passes. Use after writing or editing any .cpp/.h file under core, ui-core, platform, app, or tools, and whenever the clang-format GitHub Actions job reports violations.
---

# Keeping the tree clang-format clean

CI runs a `clang-format` job that fails the build on any violation, so a
formatting slip costs a round trip. Format before you hand work back.

## The one command

Run this on the files you touched:

```bash
clang-format -i <files you changed>
```

Whole-tree sweep, the same set CI checks:

```bash
for dir in core ui-core platform app tools; do [ -d "$dir" ] || continue; find "$dir" \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format -i; done
```

Verify exactly as CI does — silence and exit 0 means it will pass:

```bash
status=0; for dir in core ui-core platform app tools; do [ -d "$dir" ] || continue; find "$dir" \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 --no-run-if-empty clang-format --dry-run --Werror || status=1; done; exit $status
```

On macOS `xargs` has no `--no-run-if-empty`; drop that flag locally.

## Version matters

CI pins `clang-format==22.1.8` (`.github/workflows/ci.yml`, the `format` job).
Output differs between major versions, so a mismatched local binary will
"fix" files into a state CI still rejects. Check before trusting a local run:

```bash
clang-format --version
```

If it disagrees with the pin, install the pinned one (`pipx install
clang-format==22.1.8`) rather than reformatting with what happens to be on PATH.

## What the config does

`.clang-format` at the repo root: Google base, C++20, 100-column limit, 4-space
indent, left-aligned pointers (`int* p`), `IncludeBlocks: Regroup`.

Regroup is the one that surprises people. Includes are sorted into fixed
priority blocks, and the formatter *will* move them:

1. C++ standard library — `<algorithm>`, `<cstdint>`
2. `<catch2/...>`
3. Other angle includes — Qt, libav/libsw, everything else
4. `"zaro/..."` project headers
5. Everything else, e.g. `"CurveEditor.h"` in `app/`

The file's own header stays first regardless. Don't hand-place project includes
above the standard library — clang-format moves them below, and a hand-tuned
order is just a diff waiting to happen.

## Writing code it won't rewrite

Most violations in this repo are one of three things:

- **Line over 100 columns**, broken by hand at the wrong point. Let the
  formatter choose the break; don't pre-wrap long calls.
- **Continuation alignment** — hand-aligned arguments or `&&` chains that
  don't match what clang-format computes.
- **Aligned trailing initializers** in `constexpr` arrays; the column padding
  is the formatter's to decide, not yours.

In all three cases the fix is the same: write it naturally, then run
`clang-format -i` and take its answer.

## Do not fight it

There is no `// clang-format off` in this codebase, and adding one to preserve
a hand-built layout needs a real reason (a matrix literal whose shape carries
meaning, say) — not a preference about where a line breaks.
