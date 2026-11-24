# Architecture

This document is the authoritative map of the native client. The README is the
entry page; this file explains the contracts that must survive a port to a new
board or OS.

## Goals and constraints

- Embedded Linux first, desktop second.
- OpenGL ES 3.0 (or desktop OpenGL 3.3) for the 3D Puck; 2D fallback everywhere.
- 512 MB RAM target; no browser engine.
- Reuse Amp's existing pairing/auth flow; do not modify the phone-side site.
- Keep display, input, audio, HTTP/WebSocket, and secret storage replaceable.
- No Amp assets or commercial fonts in the repository.

## Module map

```text
main.cpp
  └─ App
       ├─ Platform (create_platform)
       │    ├─ Display        surface + EGL/WGL context
       │    ├─ InputSource    touch / pointer / keys
       │    ├─ AudioIO        PCM capture and playback
       │    ├─ HttpClient     pairing, session, assets
       │    ├─ WebSocketFactory  actor JSON-RPC
       │    └─ SecureStore    session cookie / actor token
       ├─ Renderer
       │    └─ Canvas         NanoVG GLES3 (Linux) / NanoVG GL3 (Windows)
       ├─ PuckView
       │    ├─ Puck2D         sprite + face curves
       │    └─ Puck3D         PUCK v2 + GLES 3.0 / GL 3.3
       └─ CarUi               pairing / car / call / media screens
```

## Replaceable seams

| Seam | Owns | M0 implementation | Embedded target | Desktop target |
| --- | --- | --- | --- | --- |
| `Display` | surface, EGL/WGL context, swap | `HeadlessDisplay` (surfaceless) | DRM/KMS + GBM | Win32 + WGL (implemented); Wayland/X11 (planned) |
| `InputSource` | pointer/touch/key events | none (headless) | libinput/evdev | Win32 messages (implemented); libinput/Wayland (planned) |
| `AudioIO` | PCM in/out | none | ALSA | WASAPI (implemented); PulseAudio/ALSA (planned) |
| `HttpClient` | HTTP + cookie jar | none | libcurl + mbedTLS/OpenSSL | libcurl (Schannel on Windows) |
| `WebSocketFactory` | actor socket | none | IXWebSocket/libwebsockets | same, OpenSSL 3 on Windows |
| `SecureStore` | cookie/token at rest | none | encrypted file / TPM | file store |
| `Canvas` | 2D drawing + text | NanoVG GLES3 | NanoVG or LVGL | NanoVG GL3 on Windows; NanoVG or Skia elsewhere |
| `PuckView` | Puck drawing | `Puck2D` | `Puck2D`/`Puck3D` | same; `Puck3D` targets GLES 3.0 and desktop GL 3.3 |

Only `create_platform()` and the backend translation units change when a new
board is added. `App`, `CarUi`, `Renderer`, and `PuckView` must not include OS
headers.

`src/platform/drm/` implements the embedded row: `DrmDisplay` (DRM/KMS + GBM +
EGL) and `DrmInput` (libinput or evdev). Both are compiled only when the build
is configured with `--backend=drm`; the headless build does not link libdrm,
GBM, libudev, or libinput.

## Frame loop

```text
while (!display->should_close()) {
  drain input events
  display->make_current()
  renderer.begin(size, dpi)
    car_ui.draw(canvas, size, time)
  renderer.end()
  display->swap()
}
```

The headless backend renders into an FBO and writes a PPM on the last frame.
A real backend swaps an EGL surface or a DRM/KMS page flip instead. The Win32
backend swaps the WGL window buffer and can write the last frame to a PPM the
same way.

## DRM/KMS backend

`--backend=drm` builds the direct-scanout backend in `src/platform/drm/`:

- `DrmDisplay` opens the first `/dev/dri/card*` that reports KMS resources (or
  the path in `PUCK_KIOSK_DRM_DEVICE`), picks the first connected connector with
  a mode and a usable CRTC, and selects that CRTC's primary plane.
- Frames render into a GBM surface through EGL's GBM platform
  (`EGL_PLATFORM_GBM_KHR`). XRGB8888 is tried first and ARGB8888 second, so a
  driver whose EGL config alpha does not match the buffer format still works.
- The first frame is a modeset (`drmModeAtomicCommit` with `ALLOW_MODESET`, or
  `drmModeSetCrtc`). Later frames are page flips, and `swap()` blocks on the
  flip-complete event, so rendering is paced by vblank. If an atomic commit is
  rejected at runtime, the backend logs why and falls back to legacy page flips.
- The connector, CRTC, and mode are saved before the first modeset and restored
  on shutdown, so exiting the kiosk gives the console back.
- `DrmInput` prefers libinput on `seat0` and falls back to raw evdev when
  libinput cannot initialize or is not installed. Touch coordinates are mapped
  to the display's logical size, a touch or left-button mouse produces
  `PointerDown`/`PointerMove`/`PointerUp`, and Escape emits `Quit`. The evdev
  fallback tracks a single touch slot; libinput handles multi-touch slots.
- The DRM backend ignores `--frames`; it runs until Escape, `SIGINT`, or
  `SIGTERM`. A smoke test stops it the same way:
  `timeout -s TERM 10 puck-kiosk --platform drm`.

Errors are reported before the first frame: no `/dev/dri/card*`, no permission
(add the user to `video`/`input` or run as root), another process holding DRM
master, no connected connector, no primary plane, or a GBM/EGL failure. None of
them crash the app.

### Hardware checklist

The orb used for development has no `/dev/dri`, so the DRM path is verified by
compilation and by its error paths only. On a real board, check:

1. The process can become DRM master: no compositor or getty owns the device,
   and the user is in `video` (or the unit runs as root). A seat manager is not
   required, but a compositor would conflict.
2. The log shows the expected connector and mode, for example
   `drm: /dev/dri/card0 HDMI-A-1 1920x1080@60Hz, atomic, GBM <driver>`.
3. If it says `legacy page flips`, the kernel or driver lacks atomic modesetting.
   The picture should still appear and flips should still be tear-free.
4. The pairing screen is stable for several minutes, `swap()` is vblank-paced,
   and no `drm:` warnings appear.
5. Touch reaches all four corners of the panel (mapping uses the mode's pixel
   size), the pointer moves, and Escape exits.
6. `Ctrl-C` or a service stop restores the previous CRTC and console.
7. On a multi-GPU board, `PUCK_KIOSK_DRM_DEVICE=/dev/dri/card1` selects the
   display controller.

## Windows backend

`--backend=win32` builds the desktop target in `src/platform/win32/`. It is a
compile-time selection like DRM; the app, renderer, and Puck code still include
no OS headers.

- `Win32Display` creates a normal `WS_OVERLAPPEDWINDOW` window whose client
  area matches `--width`/`--height`, chooses a 32-bit double-buffered pixel
  format with an 8-bit stencil buffer, and creates an OpenGL 3.3 core context
  through `wglCreateContextAttribsARB`. `--platform headless` uses the same
  path with the window hidden and writes the last frame from `GL_BACK` to a
  PPM, which is the Windows equivalent of the Linux headless smoke test.
  `--frames 0` keeps the window open until `WM_CLOSE`, `WM_DESTROY`, or Escape.
- The process is per-monitor DPI aware (v2 when the OS provides
  `SetProcessDpiAwarenessContext`). Layout coordinates stay in pixels and
  `dpi_scale()` returns 1.0, so the render size is the physical client size.
- `Win32Input` pumps the Win32 message queue and converts
  `WM_POINTERDOWN/UPDATE/UP` (touch and pen) into the existing `InputEvent`
  types. Systems without the pointer API use `WM_TOUCH` instead, and the mouse
  uses `WM_MOUSEMOVE`/`WM_LBUTTONDOWN`/`WM_LBUTTONUP`. `WM_KEYDOWN` emits
  Escape (key 27) and function keys, `WM_CHAR` emits printable characters, and
  `WM_CLOSE`/`WM_DESTROY` emit `Quit`. Client coordinates are scaled to the
  render size, so resizing the window does not shift the hit targets.
- `NanoVgCanvas` selects `nvgCreateGL3` when `PUCK_KIOSK_CANVAS_NANOVG_GL3` is
  defined. Windows' `opengl32.dll` only exports OpenGL 1.1, so
  `src/platform/win32/gl_loader.cpp` resolves the 3.3 entry points with
  `wglGetProcAddress`; NanoVG's GL3 implementation is the unmodified vendored
  `nanovg_gl.h` with those calls mapped onto the loader table. The `Canvas`
  API does not change. Puck3D uses the same table through
  `src/platform/win32/gl3_api.h`, which maps only the GL 3.x calls that
  `opengl32.dll` does not export; the OpenGL 1.1 calls stay direct.
- `WasapiAudioIO` opens the default capture and render endpoints in shared mode
  with `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` (48 kHz mono 16-bit PCM by
  default). WASAPI event threads move samples through bounded ring buffers;
  `read_capture` returns zero when no packet is ready, matching ALSA. If either
  default endpoint is missing or COM/device activation fails, the platform
  factory logs the HRESULT and falls back to the file/null backend.
- AEC is compiled out on Windows because `webrtc-audio-processing` has no
  mingw or Windows xmake package. `create_platform()` logs
  `AEC: built without webrtc-audio-processing on Windows; echo cancellation is
  disabled`. Opus and libdatachannel stay linked, so calls work without AEC.
- Dependencies: libcurl with Schannel, IXWebSocket and libdatachannel with
  OpenSSL 3, Opus, libpng/libjpeg/libwebp, and libqrencode. The local packages
  in `third_party/xmake-repo/` only override the mingw install rules for
  libdatachannel and IXWebSocket; see that directory's README.
- `Puck3D` is built for this backend. One source serves both GL flavors: with
  `PUCK_KIOSK_GL_DESKTOP` it includes the desktop GL headers and the loader
  seam, compiles `#version 330 core` shaders without the ES precision
  qualifiers, checks for OpenGL 3.3 instead of GLES 3.0, and uses
  `glClearDepth` instead of the ES-only `glClearDepthf`. The shader bodies and
  the renderer logic are shared. Any initialization failure still makes
  `create_puck_view()` return `Puck2D`.

### Build and verify

- Cross build from Linux: `xmake f -y -p mingw -a x64 -m release
  --backend=win32`, then `xmake build -y`. Use xmake's `mingw` platform rather
  than `-p windows`: the Windows dependency ports select their mingw install
  path from the target platform.
- Native build: `xmake f -y -p windows -a x64 -m release --backend=win32
  --toolchain=msvc`, then `xmake build -y`. `clang-cl` works the same way.
- Smoke run: `wine build/mingw/x64/release/puck-kiosk.exe --platform win32
  --frames 1 --out build/win32.ppm`.
- The orb verifies the WGL context, PPM output, a real pairing QR decoded with
  `zbarimg`, a scripted mute click, and Escape/WM_CLOSE. A real Windows PC must
  still verify the WASAPI endpoints, AEC-off behavior, DPI on the target panel,
  and the live actor/voice flow.

### First-run checklist

1. The log shows `win32: WGL window, GL 3.3 ...` (a newer core context is
   fine).
2. `audio backend: wasapi` rather than `file`; check the default capture and
   render devices and the Windows microphone privacy setting.
3. The pairing QR scans and phone approval completes.
4. Touch or the mouse hits all four corners of the panel and Escape closes the
   window.
5. On a high-DPI panel, pass physical `--width`/`--height`; the layout does not
   rescale automatically.

## Puck 2D and 3D

The web client is progressive enhancement: the 2D `<img>` + 2D canvas is always
present; the 3D module is imported, and only after `getContext('webgl2')`,
asset loading, and renderer construction succeed does the 3D layer fade in.
Asset load failure, shader failure, and `webglcontextlost` leave the 2D layer
visible.

The native client mirrors that contract. M0 draws a procedural placeholder; M1
swaps in the real sprite and face curves.

- `Puck2D` uses a 498×602 sprite plus the face curve data. For custom avatars,
  the 3D asset manifest's `image-<hash>.webp` is the sprite and the matching
  `puck-<hash>.json` provides `eyes`, `mouth`, `body`, and `limbs`.
- `Puck3D` parses the PUCK v2 mesh container and runs the part, eye, spotlight,
  and composite shaders on GLES 3.0 or desktop GL 3.3. The body, mouth, nose,
  and legs are re-posed from the rig on a worker thread; arms keep their
  authored scene-space geometry and use the 498×602 image texture as props.
- `create_puck_view()` tries 3D first when enabled, and returns 2D if
  initialization fails. The app never fails because 3D is unavailable.

The 3D pass is drawn into an offscreen RGBA8 + depth target sized to the
`PuckView` bounds, then composited over the 2D frame with premultiplied alpha.
`Canvas::flush()` submits the NanoVG geometry queued so far, restarts the frame,
and restores the caller's drawing state, so the 3D layer lands over the stage
background and the rest of the UI is queued on top. The offscreen target also
keeps 3D independent of the platform framebuffer's depth attachment. The dizzy
star program from the web renderer is omitted because the kiosk has no play or
morph state.

PUCK v2 container, for the M1 parser:

- Header: `PUCK`, version `2`, part count.
- Per part: 12-byte name, vertex count, index count, then `count × 16` floats
  per vertex, then 16-bit indices padded to 4 bytes.
- Rig JSON: `body`, `limbs`, `nose`, `eyes`, `mouth`, `ground`, `skinColor`,
  `scleraColor`, `material`.

Textures are 1024×1024 WebP albedo/normal maps plus a 498×602 image texture.
On a 512 MB board, decode them at 512×512 or smaller and cache only the current
and next avatar. The 3D renderer is not a reason to block the pairing screen.

## Media presentation flow

The "generated picture replaces the model" behavior is a full-stage media
overlay, not a 3D texture swap:

1. The voice session starts with `mediaKinds: ["image"]`.
2. The actor sends `puck_media_presentation`:
   `presentation` is nullable, otherwise `{ id, title?, items[1..8] }`.
3. Each item is `{ id, url, mimeType?, caption?, expiresAt?, sizeBytes? }`.
4. The car page rewrites each URL to the app origin and shows a `car-media`
   overlay: title, Close, previous/next, image.
5. The overlay covers the main stage; Puck remains mounted underneath and
   reappears when the overlay closes or `presentation` becomes `null`.

Native implementation:

- Fetch with the session cookie, decode WebP/PNG/JPEG off the UI thread.
- Cache at most a few decoded textures; honor `expiresAt`.
- Keep the mute/end controls outside the overlay and usable.
- Treat `presentation: null` as close.
- A true in-scene image plane would be a new feature, not a port of the web
  behavior.

## Pairing and auth

The car does not ask for a password. The existing Amp site owns auth:

1. Generate a 32-byte verifier; SHA-256 it to a base64url challenge.
2. Show `https://ampcode.com/auth/pair/car?c=<challenge>` as a QR code.
3. Poll `/_app/remote/1rbu0j0/claimDevicePairing` with
   `{ device: "car", verifier }` every 2 seconds.
4. The signed-in phone opens the QR link, checks the code, and approves.
5. Status goes `pending` → `scanned` (with `code`) → `paired`.
6. On `paired`, `POST /api/car/session`, then fetch `/car` with the session
   cookie to obtain `car.puck.thread` and `threadActorConfig`.

The actor connection is JSON-RPC over WebSocket. Voice uses WebRTC; the web
client creates a data channel named `puck-webrtc-voice` and sends
`client_puck_voice_start` with `{ transport: { type: "webrtc", sdp } }`.
The answer returns `{ sessionID, transport: { sdp } }`.

Actor transport facts verified against the live service:

- The car session must be renewed with `POST /api/car/session` every 30
  seconds; the server ends it after 180 seconds without activity, so a saved
  session from an exited process is usually dead and pairing must fall back
  to a fresh QR.
- `POST /api/thread-actors` returns HTTP 201 with the `wsToken`.
- The RivetKit gateway delivers JSON-RPC as binary WebSocket frames
  (`rivet_encoding.bare`); a text-only receive path sees nothing.
- With `rvt-skip-ready-wait`, the WebSocket opens before the actor wakes and
  stays silent. The client first issues `GET {gateway}/connect-ready-check`
  with the same `rvt-*` query and `Authorization: <wsToken>`; ready means
  status < 500 and no `x-rivet-error` header. Only then does the actor push
  `observerSnapshot` and further notifications.
- The actor pushes nothing until the client sends a `client_resume` JSON-RPC
  request (`params: { version }`, 0 for a full snapshot). Replies and event
  batches arrive as text frames; a batch is a JSON array of typed events.
- The wsToken lives about 180 seconds; refresh it via `/api/thread-actors`
  before expiry and reconnect the WebSocket with the new conn params.
- The car session cookie rotates on every `/api/car/session` call; persist
  the exported cookie jar after each renew, not only after pairing.

## Voice stack (M3 decision)

M3 uses libdatachannel + libopus + webrtc-audio-processing, not GStreamer
webrtcbin. A two-peer Opus loopback benchmark (DTLS-SRTP, local ICE, 20 ms
frames, 32 kbps mono, x86_64) measured peak RSS of 14.8-15.1 MB for this stack
versus 31.4-32.0 MB for webrtcbin, 16.8-17.3 MB versus 36.9-37.3 MB with AEC
enabled, and a 13-library runtime closure versus 62. Both options run the same
AEC DSP (webrtc-audio-processing, measured 20.3 dB ERLE); webrtcdsp is only a
GStreamer wrapper around it. libdatachannel callbacks are plain threads and
fit the `AudioIO` seam without a GLib main loop, and its aarch64 static cross
build is verified. The web client carries voice over a data channel named
`puck-webrtc-voice`, which is libdatachannel's core feature.

Costs accepted with this choice:

- libdatachannel has no receive jitter buffer; M3 must implement one
  (reordering, adaptive latency, NACK-driven retransmission), roughly 300-500
  lines. This is the main schedule risk, not memory.
- Pin libdatachannel v0.24.x (vendored or fetched); distros and Buildroot do
  not package it. MPL-2.0 file-level copyleft: publish patches to MPL files.
- libdatachannel does not embed ICE candidates in the SDP until gathering
  completes. Either wait for gathering done before the one-shot SDP exchange
  (the web client's pattern) or confirm the actor protocol carries candidate
  messages; a naive early SDP exchange never connects.

## Memory budget (512 MB target)

| Area | Budget |
| --- | --- |
| App + NanoVG + fonts | 10–20 MB |
| 2D Puck sprite + face | < 1 MB |
| 3D Puck (one avatar, 512px textures) | 8–15 MB |
| Media image cache (2–3 images) | 10–20 MB |
| WebRTC (libdatachannel + Opus + AEC) | 15–20 MB |
| OS + network | remaining |

The 3D renderer must release textures and mesh buffers when an avatar changes.
The media overlay must not keep every image it has shown.

## Porting checklist

1. Implement `Display` with the target's native surface (DRM/KMS, Wayland, X11).
2. Implement `InputSource` from libinput or evdev; map touch coordinates to the
   display's logical size.
3. Implement `AudioIO` with ALSA or PulseAudio; expose sample rate/channels and
   a non-blocking read/write path.
4. Implement `HttpClient` and `WebSocketFactory`; keep cookies and the actor
   token in `SecureStore`.
5. Choose the 2D `Canvas` backend: NanoVG for GLES 3.0, LVGL for a very low-end
   panel, Skia only if the RAM budget allows it.
6. Enable `Puck3D` only when GLES 3.0 and the shaders compile; otherwise keep
   `Puck2D`.
7. Add the backend to `create_platform()` and to the `--backend` option.
8. Run the headless frame first, then the target frame, then a 30-minute soak
   with a media overlay and a voice call.

## M0 status and next step

Implemented: xmake build, headless EGL/GLES, NanoVG Canvas, `Puck2D` placeholder,
pairing layout, platform interfaces.

M1: real 2D sprite/face curves, PUCK v2 parser, GLES 3.0 renderer, runtime
fallback. M2: pairing and actor. M3: voice and media overlay. M4: plugin sheets,
passkey, kiosk packaging.
