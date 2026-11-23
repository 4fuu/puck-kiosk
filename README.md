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
- A display stack on the target: DRM/KMS (planned), Wayland (planned), or X11
  (planned). The M0 build uses a headless EGL surfaceless surface.
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

## Architecture

The app is split into replaceable seams rather than one platform-specific blob:

```text
src/
  app/          CarUi, state, and the frame loop
  platform/     Display, InputSource, AudioIO, HttpClient, WebSocket, SecureStore
  render/       Canvas interface and the NanoVG GLES3 backend
  puck/         PuckView interface, Puck2D, Puck3D (M1)
  core/         types and logging
third_party/
  nanovg/       vendored MIT 2D vector renderer
```

- `Display` owns the surface/EGL context; `InputSource` owns touch/key events.
- `AudioIO` moves PCM only; WebRTC/Opus live above it.
- `Canvas` is the 2D drawing seam. NanoVG is the M0 backend; Skia, LVGL, or a
  software backend can replace it.
- `PuckView` is the 2D/3D seam. Both renderers take the same `PuckVisual` state.
- `create_platform()` is the only place that knows which OS backends exist.

See [docs/architecture.md](docs/architecture.md) for the module contracts,
Puck asset details, the media-presentation flow, and the porting checklist.

## Platform status

| Backend | Status | Notes |
| --- | --- | --- |
| headless | Works | EGL surfaceless + llvmpipe; used for CI and orb screenshots |
| DRM/KMS | Planned | Direct scanout on embedded Linux, no compositor |
| Wayland | Planned | Desktop and compositor-based embedded images |
| X11 | Planned | Desktop fallback |

The M0 binary is deliberately headless-first so the renderer and layout can be
developed without a display server. The target backend only replaces
`Display`/`InputSource`/`AudioIO`; the app and Puck code do not change.

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
- **M3:** WebRTC voice (GStreamer `webrtcbin` or libdatachannel), mute/end,
  captions, `puck_media_presentation` image overlay.
- **M4:** plugin option sheets, passkey-on-phone flow, diagnostics, DRM/KMS
  kiosk packaging, Buildroot/Yocto recipes.

## License

MIT. See [LICENSE](LICENSE).
