#pragma once

#include "platform/audio.h"

namespace pk::voice {

struct LoopbackOptions {
    int seconds = 5;
    bool aec = true;
    int reorder_every = 5;
    int drop_every = 17;
};

// Runs two VoiceEngines against each other over local ICE. Exercises the offer,
// the answer, the data channel, Opus in both directions, and the receive jitter
// buffer with induced reordering and loss. Returns 0 when every check passes.
int run_voice_loopback(AudioIO& first, AudioIO& second, const LoopbackOptions& options);

}  // namespace pk::voice
