/* Build configuration for the vendored SpeexDSP subset. Upstream generates
   this with configure; puck-kiosk always builds floating point with the
   built-in Kiss FFT. */
#ifndef PUCK_SPEEXDSP_CONFIG_H
#define PUCK_SPEEXDSP_CONFIG_H

#define EXPORT
#define FLOATING_POINT 1
#define USE_KISS_FFT 1
#define HAVE_STDINT_H 1
#define HAVE_STRING_H 1
#define HAVE_STDLIB_H 1
#define HAVE_MEMORY_H 1

#endif
