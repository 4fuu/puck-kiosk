# puck-kiosk agent notes

## Build and verify

```bash
xmake f -y -m release --backend=headless
xmake build -y
xmake run puck-kiosk --headless --frames 1 --out build/pairing.ppm
```

The headless backend uses EGL surfaceless + llvmpipe. It is the primary CI and
orb check until a DRM/KMS or Wayland backend exists.

## Boundaries

- OS/display/audio/network code belongs in `src/platform/`. The app, renderer,
  and Puck code must stay portable and must not include OS headers.
- `Canvas` is the 2D drawing seam; `PuckView` is the 2D/3D seam. Add a backend
  by implementing the interface and wiring it in `create_platform()`.
- 3D is optional. Any 3D initialization failure must fall back to `Puck2D`, not
  fail the app.
- Keep the 512 MB target in mind: decode images off the UI thread, downscale
  textures, and release avatar/media resources promptly.

## Assets

Do not commit Amp's Puck models, textures, or the PolySans / Berkeley Mono
fonts. They are fetched at runtime or supplied by the operator. See
`assets/README.md`.

The optional private mirror `4fuu/puck-kiosk-assets-private` is access-controlled.
Never make the public build depend on it, and never add it as a submodule; the
default `tools/fetch-assets` path must keep working without access.

## Current milestone

M0 is a foundation: headless EGL/GLES, NanoVG canvas, placeholder 2D Puck, and
pairing-screen layout. Pairing, actor WebSocket, WebRTC voice, and the real
PUCK v2 renderer are not implemented yet.
