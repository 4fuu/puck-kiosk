# puck-kiosk

A native C++17 client for Amp Puck car mode, built for kiosks and embedded
Linux devices with a Windows desktop build. It implements the `ampcode.com/car`
flow directly: QR pairing, Puck voice calls, media presentations, plugin option
sheets, passkey guidance, and diagnostics. It renders with OpenGL ES 3.0 on
Linux or desktop OpenGL 3.3 on Windows and runs without a browser engine. A 2D
Puck fallback keeps the app usable when the 3D assets or shaders are
unavailable.

The web client is a SvelteKit page; this client speaks the same HTTP,
WebSocket, and WebRTC protocols and keeps the same interaction model.

## Status

The car flow, voice, 2D/3D Puck, media presentations, plugin sheets, passkey
guidance, and diagnostics are implemented. These are the display and input
backends:

| Backend | What it does |
| --- | --- |
| `headless` | EGL surfaceless rendering with llvmpipe. Used for CI, tests, and frame snapshots. |
| `drm` | Direct DRM/KMS scanout through GBM and EGL, with libinput or evdev input. Compiled and error-path tested; not run on real hardware. |
| `win32` | Windows 10/11 x64 desktop window with WGL OpenGL 3.3, WASAPI audio, XInput gamepad input, a self-drawn caption bar, and a per-user NSIS installer. |
| `wayland`, `x11` | Accepted by the `--backend` option but not implemented. `create_platform()` reports an error for them. |

## Requirements

### Linux

- A C++17 compiler, `pkg-config`, and [xmake](https://xmake.io).
- EGL and OpenGL ES 3.0 development headers.
- ALSA, Opus, and `webrtc-audio-processing` for voice.
- libcurl, OpenSSL, libpng, libjpeg, libwebp, and libqrencode.
- Python 3 and `curl` for `tools/fetch-assets`.
- For `--backend=drm`: libdrm, GBM, and libudev. libinput is optional; the
  evdev fallback covers builds without it.
- 512 MB RAM is the target budget. No Chromium, WebKit, Electron, or Flutter
  runtime is used.

On Debian/Ubuntu:

```bash
sudo apt install build-essential pkg-config libegl-dev libgles-dev \
  libasound2-dev libopus-dev libwebrtc-audio-processing-dev \
  libpng-dev libwebp-dev libjpeg-dev libcurl4-openssl-dev libssl-dev \
  libqrencode-dev
```

Add `libdrm-dev libgbm-dev libudev-dev libinput-dev` for the DRM backend. The
repository's `.agents/setup` installs the full development set, including the
Windows cross-build tools.

### Windows

- Windows 10/11 x64.
- Visual Studio 2022 Build Tools with the "Desktop development with C++"
  workload, or mingw-w64 for a cross build from Linux.
- xmake.
- Python 3 and ImageMagick (`magick` or `convert`) for the generated app icon.
- NSIS (`makensis`) for the installer.
- `curl` for `tools/fetch-assets`.

The Windows build links the vendored SpeexDSP AEC, so no extra
audio-processing package is required there. A real desktop `--audio auto` or
explicit `--audio wasapi` keeps a retryable WASAPI backend when no default
endpoint exists; it does not substitute the synthetic file/tone backend.
`--headless --audio auto`, `--audio file`, and `--audio null` retain that test
backend.

## Build and run

### Linux headless

```bash
xmake f -y -m release --backend=headless
xmake build -y
xmake run puck-kiosk --headless --frames 1 --out build/pairing.ppm
```

A successful run logs the EGL/GL version and writes the frame:

```text
[puck-kiosk] info  headless EGL 1.5, GL OpenGL ES 3.2 Mesa ..., renderer llvmpipe ...
[puck-kiosk] info  wrote build/pairing.ppm (1280x720)
```

Convert the PPM if needed (ImageMagick 7 uses `magick` instead of `convert`):

```bash
convert build/pairing.ppm build/pairing.png
```

With `assets/manifest.json` present, the client verifies the cache and
downloads missing Puck assets into `assets/cache/` in the current working
directory. If the fetch cannot complete, it draws the 2D placeholder. To fetch
the development fixture before the first run:

```bash
tools/fetch-assets
```

Other sizes and the 2D-only renderer:

```bash
xmake run puck-kiosk --headless --width 720 --height 1280 \
  --frames 1 --out build/pairing-portrait.ppm
xmake run puck-kiosk --headless --frames 1 --no-3d --out build/pairing-2d.ppm
```

The DRM backend runs on a free DRM device. The actor observation window
defaults to 60 seconds; pass `--actor-seconds 0` to keep the kiosk running
until Escape, `SIGINT`, or `SIGTERM`:

```bash
xmake f -y -m release --backend=drm
xmake build -y
xmake run puck-kiosk --platform drm --actor-seconds 0
```

Set `PUCK_KIOSK_DRM_DEVICE=/dev/dri/card1` on a multi-GPU board. See
[docs/architecture.md](docs/architecture.md) for the hardware checklist.

### Windows desktop

Build with MSVC from a Developer Command Prompt:

```bat
xmake f -y -p windows -a x64 -m release --backend=win32 --toolchain=msvc
xmake build -y
xmake run puck-kiosk --platform win32
```

With no arguments the window opens, restores a saved car session, or shows the
pairing QR when there is no valid session. `--pair` forces a fresh pairing.
`--temporary-session` is the explicit shared-vehicle policy: never restore a
previous account, never persist the car session, and delete it and return to
the QR after hangup or exit. Personal desktop launches keep the restore default.
`--fullscreen` uses a borderless window with an exact `--width` x `--height`
client area; `--render-scale <1..3>` controls supersampling.

Build the per-user installer after the executable:

```bat
python tools\fetch-assets
xmake pack -f nsis
```

`tools/fetch-assets` is a Python script, so run it with `python` on Windows.
It is optional for the build but supplies the 2D sprite used for the app and
installer icon. It needs Python 3 and `curl`; the icon step needs ImageMagick.
`xmake pack` needs `makensis` on `PATH` or in the standard NSIS install
directory. The installer is written under `build\xpack\puck-kiosk\`. It
installs for the current user without administrator rights into
`%LOCALAPPDATA%\Programs\puck-kiosk`, adds Start Menu and optional desktop
shortcuts, and registers an uninstaller in HKCU. The app keeps its cookie jar
and saved car session in `%LOCALAPPDATA%\puck-kiosk`; uninstalling removes the
program, the manifest, and the re-downloadable `assets\cache`, and leaves that
session data in place.

The mingw cross build from Linux uses the same sources:

```bash
sudo apt install mingw-w64 nsis imagemagick python3 curl
tools/fetch-assets
xmake f -y -p mingw -a x64 -m release --backend=win32
xmake build -y
xmake pack -f nsis
```

Use `-p mingw`, not `-p windows`: xmake's Windows dependency ports select their
mingw install path from the target platform. The result is under
`build/mingw/x64/release/`. The hidden Windows smoke test writes a PPM:

```bash
wine build/mingw/x64/release/puck-kiosk.exe --platform headless \
  --frames 1 --out build/win32-pairing.ppm
```

On a headless Linux host, prefix the wine command with `xvfb-run -a`.

The mingw cross build is the verified Windows build path in this repository.
The native MSVC path builds the same pinned dependencies but has not been
exercised here.

## Features

- **Pairing and session restore.** A 32-byte verifier becomes a SHA-256
  base64url challenge for the phone QR. The client polls the SvelteKit remote
  endpoint, creates the car session, persists the rotating cookie jar, and
  restores a saved session on the next launch. Startup, polling, session
  renewal, token refresh, and actor socket replacement run off the UI thread.
  A failed round clears its QR and offers Try Again; `--temporary-session` is
  the explicit shared-vehicle mode that deletes the session, clears the
  in-memory cookie jar, and returns to the QR after hangup or exit.
- **Signaling recovery.** A signaling-only socket loss keeps the WebRTC media
  and the current call. Once the replacement socket receives its resume
  acknowledgement, the client reattaches with the original `sessionID`, tracks
  the receipt, and replays non-audio events missed during that call (at most
  500, each at most 16 KB). A media failure is a separate terminal state. A
  call is released only after the bounded close handshake, and an unconfirmed
  close forces a fresh socket plus an old-stop retry before redial.
- **Car screen and voice.** The car screen shows Puck, the call control, live
  captions, and the mute/end controls. Voice uses libdatachannel and Opus with
  a receive jitter buffer and NACK retransmission. AEC and noise suppression
  run through `webrtc-audio-processing` on Linux; the same capture/render seam
  uses the vendored SpeexDSP backend on Windows and as a runtime fallback.
  AGC is off by default: SpeexDSP's AGC lowered normal speech and cut echo
  return loss enhancement sharply in the file tests; APM's AGC raised quiet
  speech but still reduced cancellation.
  `On` and `Remote Only` both enable AEC because the kiosk's only playback is
  the remote voice; `Off` is a true bypass. The Debug panel reports whether the
  selected mode is actually available in the running build.
- **Puck.** `Puck2D` draws the 498x602 sprite with the face overlay and
  placeholder fallback. `Puck3D` parses the PUCK v2 meshes and rig, runs the
  part, eye, spotlight, and composite shaders on GLES 3.0 or desktop GL 3.3,
  and falls back to 2D if initialization fails. Pointer input drives gaze,
  body drag, turn drag, and throw springs.
- **Media and plugins.** The `puck_media_presentation` overlay fetches images
  with the session cookie, decodes them off the UI thread, downscales to
  1280 px, and keeps three decoded textures. Plugin `select`, `confirm`, and
  phone sheets answer `plugin.ui.respond`. The passkey flow shows phone
  guidance or re-authorizes the actor connection.
- **Input and desktop integration.** Touch, pen, mouse, keyboard, and gamepad
  input share one event model. Gamepad focus navigation includes a hint bar,
  and End is hold-to-confirm. The Windows window draws its own 36 px caption
  bar in windowed mode, with minimize, maximize, and close; Win11 gets DWM
  rounded corners. Windowed and fullscreen rendering is supersampled at up to
  24M pixels and runs at 60 fps on desktop.
- **Diagnostics.** The hidden Debug panel shows environment, audio, and session
  state, separates the next call's AEC configuration from the current call's
  reported status, runs a raw microphone test, selects the AEC mode, and posts
  the web client's diagnostics body. Entering car mode posts the usage entry. Reveal
  the panel with F12, a 2-second hold or five taps in the bottom-left corner,
  or Back+Start on a gamepad.

## Project layout

```text
src/
  app/          App state, car UI, pairing, calls, media, plugins, diagnostics
  core/         Types, logging, SHA-256
  platform/     Display, input, audio, HTTP, WebSocket, secure store
  puck/         PuckView, Puck2D, Puck3D, PUCK v2 parsing, pointer physics
  render/       Canvas interface and the NanoVG GLES3/GL3 backend
  voice/        WebRTC peer, Opus, RTP, jitter buffer, AEC
tests/          Unit and headless render tests
tools/          Asset fetcher, icon generator, NSIS installer template
assets/         Manifest only; fetched binaries are gitignored
docs/           Architecture and protocol notes
third_party/    NanoVG, Material Symbols, nlohmann/json, GL headers
```

`create_platform()` in `src/platform/platform.cpp` is the only place that
selects OS backends. The app, renderer, and Puck code include no OS headers.
See [docs/architecture.md](docs/architecture.md) for the module contracts,
pairing and actor protocols, backend details, and the porting checklist.

## Tests

| Target | Covers |
| --- | --- |
| `puck-tests` | PUCK v2 parsing, rig parsing, posing, malformed input |
| `puck-interaction-tests` | Gaze, grab, drag, throw, spring-back, reset |
| `voice-tests` | RTP, jitter buffer, NACK, Opus round-trip |
| `call-flow-tests` | Offer/answer order, ready cue, dictation gate, close confirmations, redial |
| `media-tests` | WebP/PNG/JPEG decode, downscale, texture cache eviction |
| `ui-tests` | Hit map, focus, pairing recovery, call availability, plugin sheets, passkey, diagnostics |
| `ui-render-tests` | Headless NanoVG output, caption bar, layout, empty pairing card |
| `startup-tests` | Session restore, async pairing, retry ordering, authorization, temporary-session cleanup |

Build and run all of them from a headless configure:

```bash
xmake f -y -m release --backend=headless
for t in puck-tests voice-tests call-flow-tests media-tests ui-tests \
         ui-render-tests puck-interaction-tests startup-tests voice-aec-tests; do
  xmake build -y "$t" && xmake run "$t" || exit 1
done
```

`ui-render-tests` needs the headless EGL/llvmpipe stack. The AEC smoke check
runs on the file/null audio backend:

```bash
xmake run puck-kiosk --voice-aec-smoke
xmake run voice-aec-tests
```

`voice-aec-tests` replays Puck-only, user-only, double-talk, and noise
scenarios through every compiled AEC backend. It uses two short LibriSpeech
clips under `tests/fixtures/`, reports ERLE, near-end gain/correlation, and
double-talk echo suppression, and fails when the shipped `aec+ns` path misses
its bounds. It is a file test, not a speaker/microphone measurement. Pass
`--far` and `--near` with 16 kHz or 48 kHz mono WAV recordings to replay the
same checks on an operator recording; `--wav-dir` keeps the inputs and each
backend's outputs for listening.

## Assets and licensing

This repository does not ship Amp's Puck models, textures, or the PolySans and
Berkeley Mono fonts. They are copyrighted assets. `assets/manifest.json` stores
only URLs, sizes, and SHA-256 hashes; `tools/fetch-assets` downloads the
development fixture into the gitignored `assets/cache/`.

At runtime, when `assets/manifest.json` is present in the working directory,
the client fetches missing assets from the public CDN, verifies each file
against the manifest, and caches it under `assets/cache/`. A failed fetch
leaves the existing cache in place and the renderer falls back to the
placeholder. Pass a font you have the right to use with
`--font /path/to/font.ttf`; otherwise the client uses a system font.

For a reproducible offline mirror, use a separate private repository or object
store. See [assets/README.md](assets/README.md). The optional private mirror
`4fuu/puck-kiosk-assets-private` is access-controlled; the public build and
`tools/fetch-assets` do not depend on it.

The code is MIT licensed. Third-party code under `third_party/` carries its own
license: NanoVG (MIT), Material Symbols (Apache-2.0), the Khronos GL headers
(MIT), nlohmann/json (MIT), and the SpeexDSP subset (BSD-3-Clause). The two
speech clips under `tests/fixtures/` are derived from LibriSpeech
(CC BY 4.0); see `tests/fixtures/README.md`.

## Limitations

- The DRM/KMS backend compiles and reports its error paths, but scanout has not
  been run on real hardware.
- Windows uses the vendored SpeexDSP AEC because
  `webrtc-audio-processing` is not available for that target. Its file-test
  results are in `voice-aec-tests`; real speaker, microphone, and car cabin
  crosstalk are still unverified.
- A live voice call on the company PC is still unverified. The AEC smoke test
  runs with the file/null backend; the two-peer loopback needs an ICE-capable
  network interface.
- Signaling recovery is verified against the local actor/WebRTC fixture, not a
  production actor. A dropped media connection remains a terminal call error:
  there is no automatic WebRTC rebuild after a transient `Disconnected` and no
  actor-audio relay fallback. Those behaviors, the production DataChannel name
  (`puck-webrtc-voice` versus the web's `oai-events`), and `session.closed`
  round-tripping over the online channel are unverified.
- The Windows no-endpoint path was verified under Wine: the preflight reports
  the HRESULT and the backend stays retryable. A real endpoint appearing after
  launch is not tested on Windows hardware. Real speaker/microphone double
  talk, endpoint-provided effects, and the car cabin acoustics are also
  unverified; the AEC numbers are file fixtures, not a hardware claim.
- The layout is pixel-based. On a high-DPI panel, pass `--width` and `--height`
  that match the panel; moving the window to another monitor does not re-layout
  the UI.
- The installer is unsigned, so Windows SmartScreen warns on first run.
- The mingw build links libqrencode (LGPL-2.1) statically. A permissively
  licensed encoder is required before redistribution.
- The NSIS installer ships the executable, `LICENSE.txt`, the SpeexDSP
  `LICENSE-SpeexDSP.txt`, the icon, and `assets/manifest.json`, plus whatever
  art `tools/fetch-assets` has cached at pack time, so a bundled install needs
  no first-run download. Without
  bundled art the client fetches it on first run into `assets/cache`. Uninstall
  deletes that cache; the session in `%LOCALAPPDATA%\puck-kiosk` stays.
- There is no IME or text-entry field, no platform authenticator (the passkey
  button re-authorizes the actor connection), and no kiosk lock: Escape closes
  the window.

## License

MIT. See [LICENSE](LICENSE).
