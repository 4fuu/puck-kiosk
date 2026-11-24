# puck-kiosk

A native, low-overhead Linux client for Amp Puck car mode. It targets embedded
devices with OpenGL ES 3.0 and as little as 512 MB of RAM; desktop builds use
the same core with a different window/input backend.

> **Status: M0 foundation.** The xmake build, headless EGL/GLES renderer, platform
> seams, and a placeholder pairing screen work. Pairing, network, voice, and the
> real Puck 2D/3D renderers are not implemented yet.

## Why this exists

`ampcode.com/car` is a SvelteKit car client: a pairing screen, a Puck voice call,
large driving-safe controls, plugin option sheets, and full-stage image
presentations. Running a browser engine on a 512 MB embedded board is wasteful
and often impossible. This project keeps the same interaction model but renders
it with a small C++/GLES pipeline.

The 3D Puck is optional by design. The 2D renderer is always available, and the
3D renderer falls back to it when the GLES context, assets, or shaders fail,
matching the web client's behavior when WebGL2 is unavailable.

## Requirements

- Linux (embedded or desktop), C++17 compiler, [xmake](https://xmake.io)
- EGL and OpenGL ES 3.0 development headers
- A display stack on the target: DRM/KMS (implemented, needs hardware
  verification), Wayland (planned), or X11 (planned). The default build uses a
  headless EGL surfaceless surface.
- Windows 10/11 x64 for the `win32` backend: a Win32 window, WGL, and desktop
  OpenGL 3.3 core. Build with mingw-w64 on Linux or MSVC/clang-cl on Windows.
- 512 MB RAM target; the renderer and assets are sized for that budget.
- No Chromium, WebKit, Electron, or Flutter runtime.

On Debian/Ubuntu, the development packages are installed by `.agents/setup`.
The short list is `build-essential pkg-config libegl-dev libgles-dev`.

## Quick start

```bash
xmake f -y -m release --backend=headless
xmake build -y
xmake run puck-kiosk --headless --frames 1 --out build/pairing.ppm
```

A successful run prints the EGL/GL version and:

```text
[puck-kiosk] info  Puck renderer: puck-2d
[puck-kiosk] info  wrote build/pairing.ppm (1280x720)
```

To look at the frame, convert the PPM with ImageMagick (optional):

```bash
convert build/pairing.ppm build/pairing.png
```

Portrait and other sizes are arguments:

```bash
xmake run puck-kiosk --headless --width 720 --height 1280 \
  --frames 1 --out build/pairing-portrait.ppm
```

`--no-3d` forces the 2D renderer. The default 3D option currently reports that
the GLES renderer is planned for M1 and falls back to 2D.

## Windows (win32 backend)

The Windows backend is the desktop fallback for running the kiosk client on a
company PC. It uses a Win32 window with WGL and desktop OpenGL 3.3 core, the
vendored NanoVG GL3 canvas, Win32 pointer/touch/keyboard input, and WASAPI
audio. The Linux headless, DRM, and test builds are unchanged; Windows sources
are compiled only for `--backend=win32`.

### Cross-compile with mingw-w64 (verified in this repository's orb)

```bash
sudo apt install mingw-w64
xmake f -y -p mingw -a x64 -m release --backend=win32
xmake build -y
```

The result is `build/mingw/x64/release/puck-kiosk.exe`, a Windows x64
executable with no mingw runtime DLL dependencies. Use `-p mingw` rather than
`-p windows`: xmake's Windows dependency ports branch on the target platform,
and `-p mingw` builds OpenSSL, libdatachannel, and the other packages with the
mingw toolchain instead of MSVC.

The first configure downloads and builds libcurl (Schannel), OpenSSL 3,
libdatachannel, IXWebSocket, Opus, libpng, libjpeg, libwebp, and libqrencode.
That takes several minutes and needs network access. `third_party/xmake-repo/`
contains the two local package overrides the mingw build needs; see its README.

### Run

```bash
wine build/mingw/x64/release/puck-kiosk.exe --platform win32 \
  --frames 1 --out build/win32-pairing.ppm
```

`--platform win32` opens a window and runs until `--frames` is reached, the
window is closed, or Escape is pressed. `--frames 0` runs until the window is
closed. `--platform headless` creates a hidden WGL window and writes the PPM,
which is the Windows equivalent of the Linux headless smoke test. The pairing
flow needs network access:

```bash
wine build/mingw/x64/release/puck-kiosk.exe --pair --frames 0
```

`--fullscreen` creates a borderless popup whose client area is exactly
`--width` x `--height`, centered in the primary monitor's work area. At the
monitor's native size it also covers the taskbar. Windowed mode remains the
default.

The UI font comes from `%WINDIR%\Fonts` (Segoe UI, then Arial) unless `--font`
is given. On a high-DPI display the process is per-monitor DPI aware, but the
layout is pixel-based; pass `--width` and `--height` that match the panel.

### Native Windows (MSVC or clang-cl)

Install Visual Studio 2022 Build Tools with "Desktop development with C++" and
xmake. From a Developer Command Prompt:

```bat
xmake f -y -p windows -a x64 -m release --backend=win32 --toolchain=msvc
xmake build -y
xmake run puck-kiosk --platform win32 --frames 1 --out build\win32-pairing.ppm
```

xmake builds the same pinned dependency versions with MSVC. The OpenSSL 3 port
needs a Windows Perl (Strawberry Perl or an MSYS2 Perl), which the xmake
package manager installs automatically. `clang-cl` works the same way with
`--toolchain=clang-cl`. The native MSVC path is documented from the same
sources but was not exercised in this orb; the mingw path is the verified one.

If your organization standardizes on vcpkg, install the matching static ports
(`curl[schannel]`, `openssl`, `libdatachannel[media]`, `ixwebsocket[openssl]`,
`opus`, `libpng`, `libjpeg-turbo`, `libwebp`, `libqrencode`), switch the win32
`add_requires` block in `xmake.lua` to system packages, and pass
`--pkg_searchdirs=<vcpkg>/installed/x64-windows-static`. The mingw and MSVC
paths above use xmake's own packages and are the supported ones; the vcpkg
variant has not been exercised in this orb.

### Known limitations

- WASAPI capture/render is implemented against the `AudioIO` seam, but no real
  Windows audio device was available in the orb. Under wine the default
  endpoints are missing, so the client logs the failure and falls back to the
  file/null backend. Test both directions on the target PC.
- Echo cancellation is compiled out on Windows: `webrtc-audio-processing` has
  no mingw/Windows xmake package. The startup log says
  `AEC: built without webrtc-audio-processing on Windows`. Opus and
  libdatachannel are still linked, so a call works without AEC.
- The 3D Puck is not built for `--backend=win32`; the Windows backend uses
  `Puck2D`. The Canvas is desktop GL 3.3, while `Puck3D` is a GLES 3.0
  renderer.
- DPI awareness is per-monitor v2, but `Display::dpi_scale()` returns 1.0 and
  the layout is pixel-based. The window is sharp on high-DPI screens but the
  UI is physically smaller unless `--width`/`--height` are raised. There is no
  automatic re-layout when the window moves to another monitor.
- Input is windowed: `WM_POINTER` for touch and pen, `WM_TOUCH` as the fallback
  on systems without the pointer API, `WM_MOUSE*` for the mouse, and
  `WM_KEYDOWN`/`WM_CHAR`. There is no IME or text-entry field yet, and no
  gamepad support (XInput will be wired later).
- The window is a normal resizable window by default. `--fullscreen` uses a
  borderless popup with an exact client size; Escape still quits and F12 still
  toggles Debug. There is no kiosk lock.
- Windowed mode caps a 1920x1080 client at 1920x1061 on a 1920x1080 monitor
  because of the frame and maximum tracking size. Use `--fullscreen` for an
  exact 1920x1080 client.
- `libqrencode` is LGPL-2.1. The mingw build links it statically into the
  executable, which is fine for local evaluation but needs a DLL (or a
  permissively licensed encoder) before redistribution.
- The actor WebSocket, pairing, and voice paths compile, but only pairing was
  exercised against the live site in the orb; a real Puck call needs a paired
  phone and the target PC's microphone/speakers.

### First run on the company PC

1. Start `puck-kiosk.exe --platform win32 --frames 0`. The log should show
   `win32: WGL window, GL 3.3 ...` and `audio backend: wasapi`.
2. Confirm the pairing QR scans with a phone and the phone approval completes.
3. Click a control and press Escape; the window should close.
4. If the log says `wasapi: no default playback device` or `audio backend:
   file`, check the default devices and the Windows microphone privacy setting.
5. If the UI is too small on a high-DPI panel, pass `--width`/`--height` for
   the panel's physical resolution.

## Architecture

The app is split into replaceable seams rather than one platform-specific blob:

```text
src/
  app/          CarUi, state, and the frame loop
  platform/     Display, InputSource, AudioIO, HttpClient, WebSocket, SecureStore
  render/       Canvas interface and the NanoVG GLES3/GL3 backends
  puck/         PuckView interface, Puck2D, Puck3D (M1)
  core/         types and logging
third_party/
  nanovg/       vendored MIT 2D vector renderer
```

- `Display` owns the surface/EGL context; `InputSource` owns touch/key events.
- `AudioIO` moves PCM only; WebRTC/Opus live above it.
- `Canvas` is the 2D drawing seam. NanoVG is the M0 backend (GLES3 on Linux,
  GL3 on Windows); Skia, LVGL, or a software backend can replace it.
- `PuckView` is the 2D/3D seam. Both renderers take the same `PuckVisual` state.
- `create_platform()` is the only place that knows which OS backends exist.

See [docs/architecture.md](docs/architecture.md) for the module contracts,
Puck asset details, the media-presentation flow, and the porting checklist.

## Platform status

| Backend | Status | Notes |
| --- | --- | --- |
| headless | Works | EGL surfaceless + llvmpipe; used for CI and orb screenshots |
| DRM/KMS | Implemented, unverified on hardware | Direct scanout + libinput/evdev input; build with `--backend=drm` |
| win32 | Implemented, smoke-tested under wine | Win32 + WGL OpenGL 3.3 core, NanoVG GL3, pointer/touch/mouse, WASAPI; 2D Puck only |
| Wayland | Planned | Desktop and compositor-based embedded images |
| X11 | Planned | Desktop fallback |

The M0 binary is deliberately headless-first so the renderer and layout can be
developed without a display server. The target backend only replaces
`Display`/`InputSource`/`AudioIO`; the app and Puck code do not change. The
Windows backend is the desktop evaluation path; it does not change the
embedded Linux build.

## Assets and licensing

This repository does **not** ship Amp's Puck models, textures, or the PolySans /
Berkeley Mono fonts. They are copyrighted assets. `assets/manifest.json` stores
only URLs, sizes, and hashes; `tools/fetch-assets` downloads the development
fixture into the gitignored `assets/cache/`.

```bash
tools/fetch-assets
```

At runtime the client fetches the public 3D assets on demand and caches them;
the 2D renderer uses a local sprite supplied by the operator. For a reproducible
offline mirror, use a separate private asset repository or object store. See
[assets/README.md](assets/README.md).

### Private asset mirror (optional)

The classic fixture is also mirrored in the **private** repository
[4fuu/puck-kiosk-assets-private](https://github.com/4fuu/puck-kiosk-assets-private).
Access requires an explicit invite. The public build does **not** depend on it:
`tools/fetch-assets` keeps using the public source URLs, and collaborators
without access can ignore this section and use the public fetch path above.

The code is MIT licensed. Third-party notices are in `third_party/nanovg/`.

## Roadmap

- **M0 (current):** build, headless EGL/GLES, Canvas/NanoVG, Puck2D placeholder,
  pairing-screen layout, platform seams.
- **M1:** real 2D avatar + face curves, PUCK v2 parser, GLES 3.0 Puck renderer,
  2D fallback on context/asset/shader failure.
- **M2:** QR pairing + `claimDevicePairing` poll, session cookie, `/car` data,
  actor WebSocket JSON-RPC.
- **M3:** WebRTC voice (libdatachannel + Opus + webrtc-audio-processing), mute/end,
  captions, `puck_media_presentation` image overlay.
- **M4:** plugin option sheets, passkey-on-phone flow, diagnostics, DRM/KMS
  kiosk packaging, Buildroot/Yocto recipes.

## License

MIT. See [LICENSE](LICENSE).
