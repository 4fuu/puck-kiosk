# SpeexDSP (vendored subset)

This is a source subset of [SpeexDSP 1.2.1](https://www.speex.org/) used by the
portable echo-cancellation backend. It is compiled into the kiosk on targets
that do not have `webrtc-audio-processing` (currently Windows) and is available
as a fallback when the WebRTC audio processing module cannot initialize.

Upstream tarball: `speexdsp-1.2.1.tar.gz` from
<https://downloads.xiph.org/releases/speex/>, SHA-256
`8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d`.
Licence: BSD-3-Clause, see `COPYING`.

Included files are the echo canceller (`mdf.c`), preprocessor
(`preprocess.c`), filter bank, and Kiss FFT, plus the headers they need. The
resampler, jitter buffer, and codec sources are not included. `config/` is a
puck-kiosk build configuration (floating point, Kiss FFT, empty `EXPORT`),
not an upstream file; upstream normally generates it with `configure`.
`include/speex/speexdsp_config_types.h` is likewise generated from the
upstream `.in` template.

Local changes: none to the copied sources. All settings used by the kiosk are
applied through the public `speex_echo_ctl` and `speex_preprocess_ctl` APIs.
The build defines `DISABLE_WARNINGS`, an upstream switch that suppresses
SpeexDSP's direct `stderr` warnings; the kiosk logs its own AEC backend and
state.
