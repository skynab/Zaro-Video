# Branding

Everything here except the masters is derived: `regenerate.sh <asset-directory>`
rewrites the lot from one 1024px PNG and the two hand-made `.ico` files. Run it
rather than editing these by hand when the artwork is revised.

Where each file is used. Every one of them is a build input; none is generated
during the build, so a change here is a change to a committed file.

| File | Used by |
| --- | --- |
| `CutReel-{16,32,48,64,128,256,512}.png` | The window and taskbar icon, compiled into `zaro-preview` as Qt resources under `:/branding` (see `app/CMakeLists.txt`) |
| `CutReel.ico` | The Windows executable's icon, through `app/zaro-preview.rc.in` -- and so also the desktop and Start Menu shortcuts, which take theirs from the target, and the `.zaro` document icon the MSI registers |
| `CutReel.icns` | The macOS bundle icon (`MACOSX_BUNDLE_ICON_FILE`) |
| `CutReel-Installer.ico` | The MSI's own icon: what Add/Remove Programs shows (`CPACK_WIX_PRODUCT_ICON`) |
| `installer-banner.bmp` | The strip across the top of every installer page (`CPACK_WIX_UI_BANNER`), 493x58 |
| `installer-dialog.bmp` | The background of the installer's first and last page (`CPACK_WIX_UI_DIALOG`), 493x312 |

The two BMP sizes are fixed by WiX's standard UI, not chosen, and both are
light on purpose: WixUI draws its title and body text over them in black.
