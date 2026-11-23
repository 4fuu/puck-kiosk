#pragma once

#include "voice/rtp.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pk::voice {

enum class PeerState {
    New,
    Connecting,
    Connected,
    Disconnected,
    Failed,
    Closed,
};

// A single WebRTC call leg: one Opus audio track, one puck-webrtc-voice data
// channel, and the one-shot SDP exchange. libdatachannel does ICE, DTLS, SRTP,
// SCTP, and the data channel; this class owns the RTP/RTCP details that its
// media API does not cover (Opus packetization, NACK generation, and
// retransmission).
struct VoicePeerOptions {
    // Loopback diagnostics only. 0 disables the impairment.
    int test_reorder_every = 0;
    int test_drop_every = 0;

    std::function<void(const RtpPacket&)> on_audio;
    std::function<void(const std::string&)> on_data;
    std::function<void(PeerState)> on_state;
    std::function<void(const std::string&)> on_error;
};

class VoicePeer {
public:
    explicit VoicePeer(VoicePeerOptions options);
    ~VoicePeer();
    VoicePeer(const VoicePeer&) = delete;
    VoicePeer& operator=(const VoicePeer&) = delete;

    // Offerer. Waits for ICE gathering to complete before returning the SDP,
    // because libdatachannel only embeds candidates once gathering is done.
    bool create_offer(std::string* sdp, std::string* error);

    // Answerer for the loopback harness. The answerer sends on the track that
    // the offer created, which keeps the exchange to one offer/answer.
    bool accept_offer(const std::string& offer_sdp, std::string* answer_sdp, std::string* error);
    bool accept_answer(const std::string& answer_sdp, std::string* error);

    bool send_audio(const std::uint8_t* payload, std::size_t size, std::uint32_t rtp_timestamp);
    bool send_nack(const std::vector<std::uint16_t>& sequence_numbers);
    bool send_data(const std::string& text);

    PeerState state() const;
    bool connected() const;
    void close();

    std::uint64_t sent_packets() const;
    std::uint64_t received_packets() const;
    std::uint64_t retransmitted_packets() const;
    std::uint64_t dropped_packets() const;
    std::uint32_t local_ssrc() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pk::voice
