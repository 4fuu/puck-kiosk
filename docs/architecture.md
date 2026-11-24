# Architecture

This document describes the native client as it is built today: its module
boundaries, platform backends, and the protocols it speaks. The README is the
entry page; this file is the reference for changing the code or porting it to a
new board.

## Constraints

- Native C++17 with no browser engine.
- Embedded Linux first, Windows desktop second.
- OpenGL ES 3.0 on Linux or desktop OpenGL 3.3 on Windows for the 3D Puck; a
  2D renderer is always available.
- 512 MB RAM is the target budget.
- Reuse Amp's pairing, session, actor, and voice protocols; the phone-side site
  is unchanged.
- OS, display, input, audio, network, and secret-storage code lives in
  `src/platform/`. The app, renderer, and Puck code include no OS headers.
- Amp's Puck art and the PolySans / Berkeley Mono fonts are not committed.

## Process and modules

```text
main.cpp
  └─ App
       ├─ Platform (create_platform)
       │    ├─ Display         surface + EGL/WGL context
       │    ├─ InputSource     pointer, touch, key, gamepad
       │    ├─ AudioIO         PCM capture and playback
       │    ├─ HttpClient      pairing, session, assets, diagnostics
       │    ├─ WebSocketFactory actor JSON-RPC
       │    └─ SecureStore     session cookie and car session
       ├─ Renderer
       │    └─ Canvas          NanoVG GLES3 (Linux) / NanoVG GL3 (Windows)
       ├─ PuckView
       │    ├─ Puck2D          sprite + face overlay
       │    └─ Puck3D          PUCK v2 + GLES 3.0 / GL 3.3
       ├─ CarUi                pairing / car / call / media / sheets / debug
       ├─ SessionStartup       cookie restore and session validation
       ├─ AssetFetcher         manifest verification and download
       ├─ PairingSession       challenge, poll, session, actor token
       ├─ CallController       voice JSON-RPC and call state
       ├─ MediaOverlay         image presentations
       ├─ PluginUi             plugin.ui.* requests
       ├─ PasskeyFlow          passkey guidance and actor re-authorization
       ├─ Diagnostics          Debug panel and diagnostics/usage POSTs
       ├─ UiInputRouter        pointer hit testing
       ├─ UiFocus              gamepad focus and hold-to-confirm
       └─ PuckInteraction      gaze, drag, throw physics
```

`App::run` creates the platform, renderer, a placeholder Puck, the car UI, and
the controllers. `run_session` is the live path: startup, pairing or session
restore, actor connection, car screen, and calls. `run_demo_screen` and
`run_demo_media` render one frame for UI verification and use the same drawing
code.

`create_platform()` in `src/platform/platform.cpp` is the only place that knows
which OS backends are compiled in. Adding a backend means extending that
factory and the `backend` option in `xmake.lua`, not touching `App` or the
renderers.

## Platform seams

| Seam | Owns | Linux headless | DRM/KMS | Windows |
| --- | --- | --- | --- | --- |
| `Display` | surface, context, swap | `HeadlessDisplay` (EGL surfaceless, FBO, PPM) | `DrmDisplay` (GBM + EGL, direct scanout) | `Win32Display` (WGL 3.3 core) |
| `InputSource` | pointer/touch/key/gamepad | `HeadlessInput` (no events) | `DrmInput` (libinput or evdev, evdev gamepad) | `Win32CombinedInput` (window messages + XInput) |
| `AudioIO` | PCM in/out | file/null backend | ALSA | WASAPI, file/null fallback |
| `HttpClient` | HTTP and cookie jar | libcurl + OpenSSL | libcurl + OpenSSL | libcurl + Schannel |
| `WebSocketFactory` | actor socket | IXWebSocket | IXWebSocket | IXWebSocket + OpenSSL 3 |
| `SecureStore` | cookie/session at rest | file store | file store | file store |
| `Canvas` | 2D drawing and text | NanoVG GLES3 | NanoVG GLES3 | NanoVG GL3 |
| `PuckView` | Puck drawing | `Puck2D` / `Puck3D` | `Puck2D` / `Puck3D` | `Puck2D` / `Puck3D` |

The file-backed `SecureStore` writes each value to its own `0600` file under a
`0700` directory: `$XDG_CONFIG_HOME/puck-kiosk` or `~/.config/puck-kiosk` when
those variables are set, and `build/puck-kiosk` relative to the working
directory otherwise. It stores the cookie jar and the saved car session; it is
not encrypted at rest.

## Startup and frame loop

A normal launch starts two worker threads before the first visible frame:

- `SessionStartup` restores the saved cookies and car session, validates a
  saved session with `GET /car`, and prepares the pairing challenge when the
  session cannot be reused.
- `AssetFetcher` reads `assets/manifest.json`, verifies every cached file by
  size and SHA-256, and downloads missing files.

The UI thread paints a loading state while those run. When the asset fetch
finishes, the placeholder `Puck2D` is replaced with `create_puck_view()`, which
tries 3D first and falls back to 2D. The car screen opens as soon as the saved
session is valid; a new pairing waits for the QR.

The live loop:

```text
while running:
  poll input, focus, pointer, and gamepad events
  update the Puck interaction and the call state
  media.poll()
  renew the car session every 30 s
  refresh the actor token before it expires
  ping the actor every 30 s
  if the screen is dirty or the frame interval elapsed:
    renderer.begin(size, dpi)
      car_ui.draw(canvas, size, time)
    renderer.end()
    display.swap()
```

The headless backend renders a fixed number of frames and writes the last one
as a PPM. A headless snapshot waits for startup to finish so the image is the
QR or connecting screen. The Windows window runs at about 60 fps; the DRM
backend renders on input and keeps a 10 fps idle cadence to save power.

## Pairing and session lifecycle

The car does not ask for a password. The existing Amp site owns auth.

1. Generate a 32-byte verifier and hash it with SHA-256 to a base64url
   challenge.
2. Show `https://ampcode.com/auth/pair/car?c=<challenge>` as a QR code.
3. Poll `/_app/remote/<hash>/claimDevicePairing` every 2 seconds. The
   `<hash>` is discovered from the `/car` page and its SvelteKit node modules;
   the fallback is `1rbu0j0`. The request body is
   `{ "payload": base64([{ "device": 1, "verifier": 2 }, "car",
   "<verifier>"]), "refreshes": [] }`.
4. The signed-in phone opens the QR link, checks the code, and approves.
5. Status goes `pending` -> `scanned` (with a code) -> `paired`. A challenge is
   refreshed after 240 seconds, and the screen shows the new QR.
6. On `paired`, `POST /api/car/session` with `{}` establishes the car session.
7. `GET /car` returns the page. The client parses `car.puck.thread.id` and
   `threadActorConfig` (`baseURL`, `ampURL`, `poolName`, `capability`,
   `requiresSudoForWrite`).
8. Persist the cookie jar and the car session in the `SecureStore`.

The session cookie rotates on every `POST /api/car/session`. The client renews
it every 30 seconds while the car screen is active and persists the exported
cookie jar after each renew. The service ends an idle car session after about
180 seconds, so a saved session from a process that exited long ago usually
fails validation and the client starts a fresh pairing. A `401` during a call
ends that call.

`--pair` skips the saved-session check and starts a new challenge. The
`PUCK_KIOSK_ORIGIN` environment variable overrides the origin only for a
loopback URL; it exists for local protocol testing and is ignored otherwise.

## Actor transport

After the car session is established:

- `POST /api/thread-actors?threadId=<id>&capability=<capability>` returns
  HTTP 200 or 201 with a `wsToken`. The request body is
  `{ "threadId": ..., "capability": ... }`.
- The token is a JWT. The client refreshes it about 45 seconds before `exp`,
  closes the socket, reconnects, and sends `client_resume` again. A call
  reattaches to the actor after the reconnect.
- The gateway URL is built from `threadActorConfig.baseURL`:
  `/gateway/threadActor/websocket/` with `amp-browser-page-load-id`,
  `rvt-namespace`, `rvt-method=get`, `rvt-key=<threadId>`, optional
  `rvt-token`, and `rvt-skip-ready-wait=true`.
- The WebSocket subprotocols are `rivet`, `rivet_encoding.bare`,
  `rivet_skip_ready_wait`, and `rivet_conn_params.<url-encoded JSON>`. The
  connection parameters carry `browserPageLoadID`, `transport: "json-rpc"`,
  and `wsToken`.
- With `rvt-skip-ready-wait`, the socket opens before the actor is ready and
  stays silent. The client first calls
  `GET <gateway>/connect-ready-check` with the same `rvt-*` query,
  `Authorization: <wsToken>`, and `x-rivet-conn-params`. Ready means a status
  below 500 and no `x-rivet-error` header.
- The actor pushes nothing until the client sends `client_resume` with
  `params: { "version": 0 }` for a full snapshot. Replies and event batches
  arrive as JSON; a batch is an array of typed events. The IXWebSocket receive
  path accepts both text and binary frames because the RivetKit bare encoding
  can deliver binary.
- The client sends a text `ping` every 30 seconds and ignores `pong`.

Actor requests and notifications used by the client:

| Direction | Method or event | Purpose |
| --- | --- | --- |
| out | `client_resume` | Resume the observer and request the full snapshot |
| out | `client_puck_voice_start` | Start a call with an SDP offer |
| out | `client_puck_voice_stop` | End a call |
| out | `client_puck_voice_user_state` | Publish the mute state |
| out | `client_reauthorize` | Re-authorize the actor connection for a passkey prompt |
| out | `executor_plugin_message` | Answer a `plugin.ui.respond` request |
| in | `puck_voice_state` | Call state and mute state |
| in | `puck_voice_transcript` | Live caption text |
| in | `puck_media_presentation` | Image presentation |
| in | `plugin_message` | Plugin UI snapshots and changes |

## Voice

`client_puck_voice_start` carries
`transport: { "type": "webrtc", "sdp": <offer> }` and
`mediaKinds: ["image"]`. The answer carries `sessionID` and
`transport: { "sdp": <answer> }`. The voice engine uses:

- 48 kHz mono, 20 ms frames, Opus at 32 kbps.
- One libdatachannel peer with one Opus audio track and a data channel named
  `puck-webrtc-voice`.
- RTP/RTCP handling above libdatachannel: Opus packetization, NACK generation
  and retransmission, and a receive jitter buffer with adaptive delay and
  packet-loss concealment.
- AEC, noise suppression, and a high-pass filter through
  `webrtc-audio-processing` on Linux. The render path receives every played
  frame as the AEC reference. Windows builds compile AEC out because the
  package is not available for that target.

`CallController` owns the protocol and UI state; `VoiceEngine` moves audio.
The call state machine covers offer, answer (15 s timeout), connect (15 s
timeout), active, muted, ended, and failed. Mute sends
`client_puck_voice_user_state`; end sends `session.close` on the data channel
and then `client_puck_voice_stop`. A `session.closed` data-channel message or a
remote `ended`/`closed` state ends the call. `--mute-after`, `--end-after`, and
the loopback modes are test hooks for these paths.

The stack is libdatachannel + Opus + webrtc-audio-processing rather than
GStreamer webrtcbin. A two-peer Opus loopback benchmark measured peak RSS of
14.8-15.1 MB versus 31.4-32.0 MB for webrtcbin, 16.8-17.3 MB versus
36.9-37.3 MB with AEC, and a 13-library runtime closure versus 62. libdatachannel
has no receive jitter buffer, so the client implements one.

## Puck rendering and interaction

The web client treats 3D as an enhancement: the 2D image and canvas are always
present, and the 3D module fades in only after WebGL2, assets, and renderer
construction succeed. The native client keeps that contract.

`Puck2D`:

- Loads the 498x602 sprite from
  `assets/cache/avatars/classic-<hash>.png` and falls back to the opaque WebP
  `assets/cache/3d/classic/image-<hash>.webp`.
- Reads the `eyes`, `mouth`, `skinColor`, and `scleraColor` fields from the rig
  JSON. The face overlay draws blinking eyes, pupils, and the mouth.
- Draws a procedural placeholder when the sprite is missing, so the pairing
  screen always renders.

`Puck3D`:

- Parses the PUCK v2 base and arms meshes plus the rig JSON. It re-poses the
  body, mouth, nose, and legs from the rig on a worker thread; arms keep their
  authored scene-space geometry and use the 498x602 image texture for the
  props.
- Uploads 512x512 albedo and normal textures and a 1024x1024 image texture.
- Runs the part, eye, spotlight, and composite shaders on GLES 3.0 or desktop
  GL 3.3. One source serves both flavors: `PUCK_KIOSK_GL_DESKTOP` selects
  `#version 330 core` shaders and the GL 3.3 entry-point loader.
- Draws into an offscreen RGBA8 + depth target sized to the `PuckView` stage,
  then composites over the 2D frame with premultiplied alpha. The offscreen
  target keeps 3D independent of the platform framebuffer's depth attachment.
- Falls back to `Puck2D` on any initialization failure: missing assets,
  unsupported GL version, shader failure, or an incomplete framebuffer.

PUCK v2 container:

- Header: `PUCK`, version `2`, part count.
- Per part: 12-byte name, vertex count, index count, then `vertex_count x 16`
  floats, then 16-bit indices padded to 4 bytes.
- Rig JSON: `body`, `limbs`, `nose`, `eyes`, `mouth`, `ground`, `skinColor`,
  `scleraColor`, `material`.

`PuckInteraction` ports the web client's pointer physics. A press only arms a
grab when it lands on the Puck; the drag starts after 10 px so a tap does not
move him. Body drag applies roll and squash; turn drag applies yaw and tip; a
throw enters a falling state with bounce, friction, and a spring back to home.
The app forwards pointer events to the interaction only when no UI target is
under the press.

## Media presentation

The "generated picture replaces the model" behavior is a full-stage overlay,
not a 3D texture swap.

1. The voice session starts with `mediaKinds: ["image"]`.
2. The actor sends `puck_media_presentation`; `presentation` is nullable,
   otherwise `{ id, title?, items[1..8] }`.
3. Each item is `{ id, url, kind?, mimeType?, caption?, expiresAt?,
   sizeBytes? }`. `kind: "video"` items are skipped; image items are shown.
4. The client rewrites each URL to the app origin, preserving the path and
   query, so the session cookie applies.
5. The overlay covers the main stage with a title, Close, previous/next, and
   the image. Puck remains mounted underneath and reappears when the overlay
   closes or `presentation` becomes `null`.

The implementation fetches with the session cookie, decodes WebP, PNG, or JPEG
on a worker thread, downscales the longest side to 1280 px, and keeps at most
three decoded textures in an LRU cache. Images are rejected above 24 MB,
4096 px, or 16M pixels, and `expiresAt` is checked before display. The
mute/end controls stay outside the overlay.

## Plugin UI and passkey

`PluginUi` handles the car page's plugin requests:

- Incoming `plugin_message` notifications carry `plugin.ui.snapshot`,
  `plugin.ui.request_added`, `plugin.ui.request_settled`, or
  `plugin.ui.request_removed` events. The `observer_snapshot` event's
  `plugins.uiRequests` is also accepted as a snapshot source.
- `ui.select` becomes option buttons; `ui.confirm` without fields becomes
  Yes/No; `ui.input` and `ui.confirm` with fields become a phone sheet.
- Answers go out as
  `{ "jsonrpc": "2.0", "method": "executor_plugin_message", "params": {
  "message": { "type": "request", "id": "plugin-ui-respond-<id>-<n>",
  "method": "plugin.ui.respond", "params": { "requestId": ..., "result": ... }
  } } }`.
- Dismissing a select sends `plugin.ui.respond` without a result. Dismissing a
  confirm or acknowledging a phone sheet only hides it, as on the web.

`PasskeyFlow` shows phone guidance when the actor is connected read-only on a
car session. The local variant appears when there is no car session but the
actor requires sudo. The kiosk has no platform authenticator, so the local
button sends `client_reauthorize` with the current `wsToken` and stays in
`Confirming` until a later actor/session update reports write access. A
rejected request switches to `Failed` with the error.

## Diagnostics and Debug

`Diagnostics` collects the viewport, DPI, backend, touch capability, audio
backend, session state, and call state. The Debug panel shows those rows, lets
the operator choose `on`, `remote-only`, or `off` echo cancellation for the
next call, runs a 0.75-second microphone test, and posts the web client's body
to `/api/car/diagnostics`. Entering car mode posts `/api/car/usage` with
`entry: "direct"` and the coarse-pointer and viewport fields.

The Debug entry is hidden in normal use. It appears after any of:

- F12.
- A 2-second hold in the bottom-left corner.
- Five taps in the bottom-left corner within 2.5 seconds.
- Back+Start held for 1.5 seconds on a gamepad.

It hides again when the panel closes.

## Input, focus, and window chrome

All backends emit the same `InputEvent` types: pointer down/move/up, key
down/up, gamepad button down/up, gamepad axis, and quit. The standard gamepad
mapping is A/B/X/Y, D-pad, Start, Back, and the bumpers; sticks are normalized
to [-1, 1].

`UiInputRouter` turns a press and release inside the same hit rect into one
action. `UiFocus` provides gamepad navigation over the hit map: D-pad and left
stick move focus, A activates, B dismisses an overlay, Start toggles Debug, and
End is hold-to-confirm for 0.9 seconds. A gamepad event shows the corner hint
bar; pointer activity hides it.

`CarUi` draws the client caption strip when the backend reports
`needs_client_caption()`. The strip is 36 px high with three 40x28 px buttons
for minimize, maximize, and close, plus a drag region. Input below the strip is
reported in content space, so the app's layout and hit map share one coordinate
system.

## Linux backends

### Headless

`--backend=headless` builds `HeadlessDisplay` and `HeadlessInput`. The display
creates an EGL surfaceless context, renders into an FBO, and writes the last
frame as a binary PPM. It is the CI and headless test path and does not link
libdrm, GBM, libudev, or libinput. `--frames 1 --out <path>` is the smoke test;
the default is one frame and `build/pairing.ppm`.

### DRM/KMS

`--backend=drm` builds the direct-scanout backend in `src/platform/drm/`.

- `DrmDisplay` opens the first `/dev/dri/card*` with KMS resources, or the path
  in `PUCK_KIOSK_DRM_DEVICE`, picks the first connected connector with a mode
  and a usable CRTC, and selects that CRTC's primary plane.
- Frames render into a GBM surface through EGL's GBM platform. XRGB8888 is
  tried first and ARGB8888 second.
- The first frame is a modeset (`drmModeAtomicCommit` with `ALLOW_MODESET`, or
  `drmModeSetCrtc`). Later frames are page flips, and `swap()` blocks on the
  flip-complete event. A rejected atomic commit falls back to legacy page
  flips.
- The connector, CRTC, and mode are saved before the first modeset and restored
  on shutdown.
- `DrmInput` prefers libinput on `seat0` and falls back to raw evdev. Touch
  coordinates are mapped to the display size; a touch or left-button mouse
  produces pointer events; Escape emits `Quit`. The evdev fallback tracks a
  single touch slot. A separate evdev reader handles gamepads.
- The DRM backend ignores `--frames`. Its actor observation window defaults to
  60 seconds; `--actor-seconds 0` keeps it running until Escape, `SIGINT`, or
  `SIGTERM`.

Errors are reported before the first frame: no card, no permission, another
process holding DRM master, no connected connector, no primary plane, or a
GBM/EGL failure. None of them crash the app.

Hardware checklist:

1. The process can become DRM master: no compositor or getty owns the device,
   and the user is in `video` or the unit runs as root.
2. The log shows the connector and mode, for example
   `drm: /dev/dri/card0 HDMI-A-1 1920x1080@60Hz, atomic, GBM <driver>`.
3. `legacy page flips` means the kernel or driver lacks atomic modesetting;
   the picture should still appear and flips should still be tear-free.
4. The pairing screen is stable for several minutes, `swap()` is vblank-paced,
   and no `drm:` warnings appear.
5. Touch reaches all four corners, the pointer moves, and Escape exits.
6. `Ctrl-C` or a service stop restores the previous CRTC and console.
7. On a multi-GPU board, `PUCK_KIOSK_DRM_DEVICE=/dev/dri/card1` selects the
   display controller.

This path is verified by compilation and its error paths, not by scanout on a
panel.

## Windows backend

`--backend=win32` builds `src/platform/win32/`. It is a compile-time selection
like DRM; the app, renderer, and Puck code still include no OS headers.

### Display

`Win32Display` creates a borderless `WS_POPUP` window. A hidden window
(`--platform headless`) renders a PPM for the Windows smoke test. A visible
window is the normal desktop app and writes no PPM unless `--out` is given.
`--fullscreen` uses an exact `--width` x `--height` client area centered in the
primary monitor's work area; at the monitor's native size it also covers the
taskbar.

The context is a 32-bit double-buffered WGL OpenGL 3.3 core context with a
24-bit depth and 8-bit stencil buffer. `src/platform/win32/gl_loader.cpp`
resolves the 3.3 entry points with `wglGetProcAddress`; NanoVG and Puck3D use
the same loader table. `wglSwapIntervalEXT(1)` paces the visible window.

The process is per-monitor DPI aware (v2 when available). Layout coordinates
stay in client pixels. Windowed and fullscreen rendering is supersampled:
`Win32Display` allocates RGBA8 color and depth-stencil renderbuffers at
`client size * effective_scale`, binds that framebuffer in `make_current()`,
and `swap()` downsamples it to the default framebuffer with a linear blit.
`effective_scale` is `--render-scale` (default 2 on a visible window, 1 hidden)
times `dpi/96`, capped by a 24M-pixel budget. `display.size()` stays the client
size; `dpi_scale()` reports the supersample factor so NanoVG and Puck3D render
at that density. `--render-scale 1` renders 1:1.

### Input and audio

`Win32CombinedInput` reads window messages first, then XInput gamepad events.
`Win32Input` converts `WM_POINTERDOWN/UPDATE/UP` (touch and pen),
`WM_TOUCH` (fallback when the pointer API is absent), and
`WM_MOUSEMOVE`/`WM_LBUTTONDOWN`/`WM_LBUTTONUP` into pointer events.
`WM_KEYDOWN` emits Escape and function keys, `WM_CHAR` emits printable
characters, and `WM_CLOSE`/`WM_DESTROY` emit `Quit`. Client coordinates are
scaled to the render size, so resizing does not shift hit targets.

`XInputGamepad` polls slots 0-3 on a background thread. XInput is loaded
dynamically (`xinput1_4.dll`, then `xinput9_1_0.dll`) so the executable still
starts on systems without the newer DLL.

`WasapiAudioIO` opens the default capture and render endpoints in shared mode
with `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` (48 kHz mono 16-bit PCM). Event
threads move samples through bounded ring buffers; `read_capture` returns zero
when no packet is ready, matching ALSA. If either endpoint is missing or COM
activation fails, the platform factory logs the HRESULT and falls back to the
file/null backend.

AEC is compiled out on Windows. `create_platform()` logs
`AEC: built without webrtc-audio-processing on Windows; echo cancellation is
disabled`. Opus and libdatachannel stay linked, so calls work without AEC.

### Caption bar and window controls

The visible window has no OS title bar. In windowed mode, `CarUi` draws a
36 px caption strip with the title "Puck Kiosk" and minimize, maximize, and
close buttons. The strip is a drag region except for the buttons.
Double-clicking the strip toggles maximize. Fullscreen mode omits the strip.
The window is rounded with DWM's corner preference on Windows 11 and falls
back to a round-rect window region on older Windows; fullscreen and maximized
windows are square. Escape quits, F12 toggles Debug, and there is no kiosk
lock.

### Installer

`xmake pack -f nsis` builds a per-user NSIS installer from the win32 target.
Packaging lives outside the app:

- `xmake.lua` declares `xpack("puck-kiosk")` with the `nsis` format, binds the
  `puck-kiosk` target, and supplies the title, version, and license.
- `tools/installer/puck-kiosk.nsi` is the checked-in template.
  `tools/installer/package.lua` substitutes the `${PACKAGE_*}` variables and
  runs `makensis`. xmake's built-in NSIS plugin returns immediately on a
  non-Windows host, so the package script invokes `makensis` directly; the
  command works from the Linux mingw cross build and from Windows.
- The template uses `RequestExecutionLevel user`,
  `$LOCALAPPDATA\Programs\puck-kiosk` as the default directory, and an HKCU
  uninstall entry. xmake's built-in template installs into `Program Files` and
  elevates unless started with `/NOADMIN`, so it is not used unchanged.
- `tools/make-icon` renders the executable and installer icon from the fetched
  2D sprite at build time: Puck mirrored to face left on a rounded tile filled
  with the app's `#2f372f` to `#1a231c` gradient. The `.ico` and the generated
  `.rc` live in the gitignored `.cache/win32/`; `windres`/`rc` embeds the icon
  in the executable. Without the sprite the rule adds no resource, logs a note,
  and the build and installer succeed without an icon. The icon is not
  committed because it derives from Amp's art.
- Both shortcuts start `puck-kiosk.exe` with no arguments and the install
  directory as the working directory, so a normal launch restores the saved
  session or starts pairing. The uninstaller removes the shortcuts, the
  executable, the icon, and the registry entries, and keeps the user's session
  and cache data.
- The installer ships the executable, `LICENSE.txt`, and the icon. It does not
  ship `assets/manifest.json` or the asset cache, so an installed copy uses the
  placeholder Puck until those files are supplied in its working directory.

The installer is unsigned.

## Assets and memory budget

`assets/manifest.json` is the only committed asset metadata: a source base URL
and, per file, a relative path, URL, SHA-256, and byte count. The fetcher
rejects absolute paths, `..`, non-HTTP URLs, and files without a valid hash.
The CDN serves some assets gzip-compressed, so the fetcher decodes
gzip/deflate before verifying the hash.

Cache layout:

```text
assets/cache/3d/base-<hash>.bin
assets/cache/3d/classic/puck-<hash>.json
assets/cache/3d/classic/albedo-<hash>.webp
assets/cache/3d/classic/normal-<hash>.webp
assets/cache/3d/classic/image-<hash>.webp
assets/cache/3d/classic/arms-<hash>.bin
assets/cache/avatars/classic-<hash>.png
```

The cache is relative to the process working directory. Treat it as
disposable; the fetcher recreates it from the manifest. The 2D renderer uses
the PNG sprite when present and the WebP image otherwise.

Memory budget:

| Area | Budget |
| --- | --- |
| App + NanoVG + fonts | 10-20 MB |
| 2D sprite + face | < 1 MB |
| 3D Puck (classic, 512 px textures) | ~1.4 MB buffers + ~3.3 MB textures + offscreen target |
| Media image cache (3 images at 1280 px) | up to ~20 MB |
| WebRTC (libdatachannel + Opus + AEC) | 15-20 MB |
| OS + network | remaining |

The 3D renderer releases its textures and mesh buffers when it is destroyed.
The media overlay releases evicted textures and clears its cache on close.

## Tests

| Target | Covers |
| --- | --- |
| `puck-tests` | PUCK v2, rig parsing, posing, malformed input |
| `puck-interaction-tests` | Gaze, grab, drag, throw, spring-back, reset |
| `voice-tests` | RTP, jitter buffer, NACK, Opus round-trip |
| `media-tests` | WebP/PNG/JPEG decode, downscale, cache eviction |
| `ui-tests` | Hit map, focus, plugin sheets, passkey, diagnostics |
| `ui-render-tests` | Headless NanoVG output, caption bar, layout |
| `startup-tests` | Session restore, pairing startup, remote hash discovery |

Build and run them after a headless configure:

```bash
xmake f -y -m release --backend=headless
for t in puck-tests voice-tests media-tests ui-tests ui-render-tests \
         puck-interaction-tests startup-tests; do
  xmake build -y "$t" && xmake run "$t" || exit 1
done
```

`ui-render-tests` needs the headless EGL/llvmpipe stack. The AEC smoke mode is
a CLI check:

```bash
xmake run puck-kiosk --voice-aec-smoke
```

`--voice-loopback` runs two local WebRTC peers and needs a network interface
that libjuice can gather ICE candidates from.

## Porting checklist

1. Implement `Display` with the target's native surface (DRM/KMS, Wayland,
   X11, or a board-specific API).
2. Implement `InputSource` from libinput, evdev, or the target's input API;
   map touch coordinates to the display's logical size.
3. Implement `AudioIO` with ALSA, PulseAudio, or the board's audio API; expose
   sample rate, channels, and a non-blocking read/write path.
4. Reuse `HttpClient`, `WebSocketFactory`, and `SecureStore`, or implement
   them for the platform.
5. Choose the `Canvas` backend: NanoVG for GLES 3.0, LVGL for a very low-end
   panel, Skia only if the RAM budget allows it.
6. Enable `Puck3D` only when GLES 3.0 and the shaders compile; otherwise keep
   `Puck2D`.
7. Add the backend to `create_platform()` and to the `--backend` option.
8. Run the headless frame first, then the target frame, then a 30-minute soak
   with a media overlay and a voice call.
