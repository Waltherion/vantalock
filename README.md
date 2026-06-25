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

> **Status: early development (working prototype).**
> Locks all outputs and shows the active vantapaper wallpaper — blurred + dimmed
> with a sharp thumbnail, the clock/date, and a password field — in true HDR per
> monitor. Authenticates via PAM and unlocks on the correct password. Not yet
> wired into the idle/lock flow, and the look is still being refined.

## Requirements

- Hyprland (or any compositor implementing `ext-session-lock-v1`,
  `wp-color-management-v1`, and a Vulkan scRGB swapchain). NVIDIA RTX + Wayland
  is the reference setup.
- Vulkan loader + ICD, `wayland-protocols`, `wayland-scanner`, `glslc`
  (shaderc), CMake ≥ 3.21, Ninja.
- Qt6 Gui (decode + text only — VantaLock never opens a Qt window), the HDR image
  libraries (libavif, libjxl, libheif, libultrahdr, lcms2), plus `libpam` and
  `libxkbcommon`.

## Authentication (PAM)

VantaLock authenticates the current user via PAM using the service named by
`VANTALOCK_PAM_SERVICE` (default `vantalock`). Install the bundled service file:

```sh
sudo install -Dm644 pam/vantalock /etc/pam.d/vantalock
```

(`make install` / the package does this for you.)

## Build

```sh
cmake -B build -G Ninja
ninja -C build
```

## Usage

```sh
# Safe, NON-LOCKING check: confirm the GPU exposes an scRGB HDR swapchain.
./build/vantalock --probe

# Lock the session (defaults to the active vantapaper wallpaper). Type your
# password and press Enter to unlock.
./build/vantalock [path/to/image]
```

### ⚠️ Testing safely

`ext-session-lock-v1` is a *real* lock: if the client dies while the session is
locked, the compositor keeps it locked (by design). While iterating, **test with
the auto-unlock safety net** so a crash or input failure can never strand your
session:

```sh
VANTALOCK_TIMEOUT=60 ./build/vantalock
```

This unlocks automatically after 60 seconds regardless of input. Keep a TTY
(Ctrl+Alt+F2) handy the first time anyway.

### Environment variables

- `VANTALOCK_TIMEOUT=<seconds>` — auto-unlock after N seconds (testing safety net).
- `VANTALOCK_PAM_SERVICE=<name>` — PAM service to authenticate against (default `vantalock`).
- `VANTALOCK_BLUR=<uv-radius>` — background blur radius (default `0.02`; `0` = sharp).
- `VANTALOCK_DIM=<0..1>` — background dim multiplier (default `0.5`; `1` = none).
- `VANTALOCK_THUMB=0` — disable the sharp thumbnail; `VANTALOCK_THUMB_HEIGHT=<frac>` sizes it.
- `VANTALOCK_CM_TAG=1` — force a manual `wp-color-management` tag (if blacks look grey).
- `VANTALOCK_VK_VALIDATION=1` — enable Vulkan validation layers.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
