# VantaLock

An HDR-capable Wayland lock screen with **true black on OLED**, built for
Hyprland. It locks the session via `ext-session-lock-v1` and renders with raw
Vulkan (scRGB extended-linear swapchain) while tagging each surface
`windows-scRGB` through `wp-color-management-v1` — so blacks stay at 0 nits
instead of being lifted to grey like SDR lock screens.

VantaLock is the lock-screen sibling of
[vantapaper](https://github.com/Waltherion/vantapaper) (HDR wallpaper daemon)
and [vantaviewer](https://github.com/Waltherion/vantaviewer) (HDR image viewer),
and reuses their proven HDR decode + colour pipeline.

> **Status: early development (Fase 0 — de-risk spike).**
> Locks all outputs, decodes one HDR image (the active vantapaper wallpaper by
> default) and presents it in true HDR; **Esc unlocks**. There is **no
> authentication yet** — do not use it as your real lock screen.

## Requirements

- Hyprland (or any compositor implementing `ext-session-lock-v1`,
  `wp-color-management-v1`, and a Vulkan scRGB swapchain). NVIDIA RTX + Wayland
  is the reference setup.
- Vulkan loader + ICD, `wayland-protocols`, `wayland-scanner`, `glslc`
  (shaderc), CMake ≥ 3.21, Ninja.
- Qt6 Gui (decode only — VantaLock never opens a Qt window), plus the HDR image
  libraries: libavif, libjxl, libheif, libultrahdr, lcms2.

## Build

```sh
cmake -B build -G Ninja
ninja -C build
```

## Usage (Fase 0)

```sh
# Safe, NON-LOCKING check: confirm the GPU exposes an scRGB HDR swapchain.
./build/vantalock --probe

# Lock the session and show an image (defaults to the active vantapaper
# wallpaper). Press Esc to unlock.
./build/vantalock [path/to/image]
```

### ⚠️ Testing safely

`ext-session-lock-v1` is a *real* lock: if the client dies while the session is
locked, the compositor keeps it locked (by design). Until authentication exists,
**always test with the auto-unlock safety net** so a crash or input failure can
never strand your session:

```sh
VANTALOCK_TIMEOUT=20 ./build/vantalock
```

This unlocks automatically after 20 seconds regardless of input. Keep a TTY
(Ctrl+Alt+F2) handy the first time anyway.

Environment variables:

- `VANTALOCK_TIMEOUT=<seconds>` — auto-unlock after N seconds (testing safety net).
- `VANTALOCK_VK_VALIDATION=1` — enable Vulkan validation layers.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
