# Architecture

This document is the authoritative map of the native client. The README is the
entry page; this file explains the contracts that must survive a port to a new
board or OS.

## Goals and constraints

- Embedded Linux first, desktop second.
- OpenGL ES 3.0 for the 3D Puck; 2D fallback everywhere.
- 512 MB RAM target; no browser engine.
- Reuse Amp's existing pairing/auth flow; do not modify the phone-side site.
- Keep display, input, audio, HTTP/WebSocket, and secret storage replaceable.
- No Amp assets or commercial fonts in the repository.

## Module map

```text
main.cpp
  └─ App
       ├─ Platform (create_platform)
       │    ├─ Display        surface + EGL/GL context
       │    ├─ InputSource    touch / pointer / keys
       │    ├─ AudioIO        PCM capture and playback
       │    ├─ HttpClient     pairing, session, assets
       │    ├─ WebSocketFactory  actor JSON-RPC
       │    └─ SecureStore    session cookie / actor token
       ├─ Renderer
       │    └─ Canvas         NanoVG GLES3 in M0
       ├─ PuckView
       │    ├─ Puck2D         sprite + face curves
       │    └─ Puck3D         PUCK v2 + GLES 3.0 (M1)
       └─ CarUi               pairing / car / call / media screens
```

## Replaceable seams

| Seam | Owns | M0 implementation | Embedded target | Desktop target |
| --- | --- | --- | --- | --- |
| `Display` | surface, EGL context, swap | `HeadlessDisplay` (surfaceless) | DRM/KMS + GBM | Wayland/X11 |
| `InputSource` | pointer/touch/key events | none (headless) | libinput/evdev | libinput/Wayland |
| `AudioIO` | PCM in/out | none | ALSA | PulseAudio/ALSA |
| `HttpClient` | HTTP + cookie jar | none | libcurl + mbedTLS/OpenSSL | libcurl |
| `WebSocketFactory` | actor socket | none | IXWebSocket/libwebsockets | same |
| `SecureStore` | cookie/token at rest | none | encrypted file / TPM | keyring |
| `Canvas` | 2D drawing + text | NanoVG GLES3 | NanoVG or LVGL | NanoVG or Skia |
| `PuckView` | Puck drawing | `Puck2D` | `Puck2D`/`Puck3D` | same |

Only `create_platform()` and the backend translation units change when a new
board is added. `App`, `CarUi`, `Renderer`, and `PuckView` must not include OS
headers.

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
A real backend swaps an EGL surface or a DRM/KMS page flip instead.

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
- `Puck3D` parses the PUCK v2 mesh container and runs the part, eye, and
  spotlight GLES shaders. The body, mouth, nose, and legs are re-posed from the
  rig on a worker thread; arms keep their authored scene-space geometry and use
  the 498×602 image texture as props.
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
