#include "voice/peer_connection.h"

#include "core/log.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <random>
#include <utility>

namespace pk::voice {
namespace {

constexpr const char* kDataChannelLabel = "puck-webrtc-voice";
constexpr std::uint8_t kOpusPayloadType = 111;
constexpr std::size_t kSendHistoryPackets = 128;
constexpr auto kGatheringTimeout = std::chrono::seconds(2);

const char* state_name(PeerState state) {
    switch (state) {
        case PeerState::New:
            return "new";
        case PeerState::Connecting:
            return "connecting";
        case PeerState::Connected:
            return "connected";
        case PeerState::Disconnected:
            return "disconnected";
        case PeerState::Failed:
            return "failed";
        case PeerState::Closed:
            return "closed";
    }
    return "unknown";
}

std::size_t count_candidates(const std::string& sdp) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = sdp.find("a=candidate:", offset)) != std::string::npos) {
        ++count;
        offset += 12;
    }
    return count;
}

std::uint32_t random_ssrc() {
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<std::uint32_t> distribution;
    return distribution(generator);
}

}  // namespace

struct VoicePeer::Impl {
    explicit Impl(VoicePeerOptions peer_options)
        : options(std::move(peer_options)), ssrc(random_ssrc()) {}

    VoicePeerOptions options;
    const std::uint32_t ssrc;

    mutable std::mutex mutex;
    std::condition_variable gathering_cv;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::DataChannel> data_channel;
    bool gathering_complete = false;
    std::atomic<PeerState> state{PeerState::New};
    std::atomic<bool> connected{false};
    std::atomic<std::uint32_t> remote_ssrc{0};
    std::atomic<std::uint64_t> local_candidates{0};
    std::atomic<std::uint64_t> sent_packets{0};
    std::atomic<std::uint64_t> received_packets{0};
    std::atomic<std::uint64_t> retransmitted_packets{0};
    std::atomic<std::uint64_t> dropped_packets{0};

    std::mutex send_mutex;
    std::uint16_t sequence = 0;
    std::uint64_t send_attempts = 0;
    std::optional<std::vector<std::uint8_t>> pending_reorder;
    std::deque<std::pair<std::uint16_t, rtc::binary>> send_history;

    bool create_peer(std::string* error) {
        try {
            rtc::Configuration config;
            config.disableAutoNegotiation = true;
            auto peer = std::make_shared<rtc::PeerConnection>(config);

            peer->onStateChange([this](rtc::PeerConnection::State value) {
                PeerState mapped = PeerState::New;
                switch (value) {
                    case rtc::PeerConnection::State::New:
                        mapped = PeerState::New;
                        break;
                    case rtc::PeerConnection::State::Connecting:
                        mapped = PeerState::Connecting;
                        break;
                    case rtc::PeerConnection::State::Connected:
                        mapped = PeerState::Connected;
                        break;
                    case rtc::PeerConnection::State::Disconnected:
                        mapped = PeerState::Disconnected;
                        break;
                    case rtc::PeerConnection::State::Failed:
                        mapped = PeerState::Failed;
                        break;
                    case rtc::PeerConnection::State::Closed:
                        mapped = PeerState::Closed;
                        break;
                }
                state = mapped;
                connected = mapped == PeerState::Connected;
                log::info("voice: peer state %s", state_name(mapped));
                if (options.on_state) {
                    options.on_state(mapped);
                }
            });

            peer->onGatheringStateChange([this](rtc::PeerConnection::GatheringState value) {
                if (value == rtc::PeerConnection::GatheringState::Complete) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        gathering_complete = true;
                    }
                    gathering_cv.notify_all();
                    log::info("voice: ICE gathering complete (%llu local candidates)",
                              static_cast<unsigned long long>(local_candidates.load()));
                }
            });

            peer->onLocalCandidate([this](rtc::Candidate) { ++local_candidates; });
            peer->onTrack([this](std::shared_ptr<rtc::Track> incoming) {
                setup_track(std::move(incoming));
            });
            peer->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
                if (!channel || channel->label() != kDataChannelLabel) {
                    if (channel) {
                        channel->close();
                    }
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (data_channel) {
                        return;
                    }
                    data_channel = channel;
                }
                setup_data_channel(channel);
            });

            std::lock_guard<std::mutex> lock(mutex);
            pc = std::move(peer);
            return true;
        } catch (const std::exception& exception) {
            if (error) {
                *error = std::string("cannot create peer connection: ") + exception.what();
            }
            return false;
        }
    }

    void setup_track(std::shared_ptr<rtc::Track> incoming) {
        if (!incoming) {
            return;
        }
        if (incoming->description().type() != "audio") {
            incoming->close();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!track) {
                // Send on the first audio track. Additional audio tracks are
                // still wired for receive: some answers put the server's audio
                // on a new m-line instead of the offered one.
                track = incoming;
            }
        }
        incoming->onOpen([this] { log::info("voice: audio track open"); });
        incoming->onClosed([this] { log::info("voice: audio track closed"); });
        incoming->onError([this](std::string error) { report_error("audio track: " + error); });
        incoming->onMessage([this](rtc::message_variant data) {
            if (auto* binary = std::get_if<rtc::binary>(&data)) {
                handle_media(reinterpret_cast<const std::uint8_t*>(binary->data()), binary->size());
            }
        });
    }

    void setup_data_channel(const std::shared_ptr<rtc::DataChannel>& channel) {
        channel->onOpen([this] { log::info("voice: data channel open"); });
        channel->onClosed([this] { log::info("voice: data channel closed"); });
        channel->onError([this](std::string error) { report_error("data channel: " + error); });
        channel->onMessage([this](rtc::message_variant data) {
            if (auto* text = std::get_if<rtc::string>(&data)) {
                if (options.on_data) {
                    options.on_data(*text);
                }
            }
        });
    }

    void handle_media(const std::uint8_t* data, std::size_t size) {
        if (!data || size == 0) {
            return;
        }
        if (is_rtcp(data, size)) {
            handle_rtcp(data, size);
            return;
        }
        RtpPacket packet;
        if (!parse_rtp(data, size, &packet)) {
            return;
        }
        if (packet.payload_type != kOpusPayloadType) {
            return;
        }
        if (remote_ssrc.load() == 0) {
            remote_ssrc = packet.ssrc;
        }
        ++received_packets;
        if (options.on_audio) {
            options.on_audio(packet);
        }
    }

    void handle_rtcp(const std::uint8_t* data, std::size_t size) {
        RtcpNack nack;
        if (!parse_rtcp_nack(data, size, &nack)) {
            return;
        }
        std::lock_guard<std::mutex> send_lock(send_mutex);
        std::shared_ptr<rtc::Track> output;
        {
            std::lock_guard<std::mutex> lock(mutex);
            output = track;
        }
        if (!output || !output->isOpen()) {
            return;
        }
        for (std::uint16_t sequence : nack.sequence_numbers) {
            for (auto it = send_history.rbegin(); it != send_history.rend(); ++it) {
                if (it->first != sequence) {
                    continue;
                }
                try {
                    if (output->send(it->second)) {
                        ++retransmitted_packets;
                    }
                } catch (const std::exception& exception) {
                    report_error(std::string("RTCP retransmit failed: ") + exception.what());
                }
                break;
            }
        }
    }

    bool wait_for_gathering(const std::shared_ptr<rtc::PeerConnection>& peer,
                            std::string* error) {
        std::unique_lock<std::mutex> lock(mutex);
        if (gathering_complete ||
            peer->gatheringState() == rtc::PeerConnection::GatheringState::Complete) {
            return true;
        }
        if (!gathering_cv.wait_for(lock, kGatheringTimeout, [this] { return gathering_complete; })) {
            if (error) {
                *error = "ICE gathering did not complete in time";
            }
            return false;
        }
        return true;
    }

    bool send_packet_locked(const std::vector<std::uint8_t>& packet, std::uint16_t sequence) {
        std::shared_ptr<rtc::Track> output;
        {
            std::lock_guard<std::mutex> lock(mutex);
            output = track;
        }
        if (!output || !output->isOpen()) {
            ++dropped_packets;
            return false;
        }
        rtc::binary binary(packet.size());
        std::memcpy(binary.data(), packet.data(), packet.size());
        try {
            if (!output->send(binary)) {
                ++dropped_packets;
                return false;
            }
        } catch (const std::exception& exception) {
            report_error(std::string("audio send failed: ") + exception.what());
            ++dropped_packets;
            return false;
        }
        send_history.emplace_back(sequence, std::move(binary));
        while (send_history.size() > kSendHistoryPackets) {
            send_history.pop_front();
        }
        ++sent_packets;
        return true;
    }

    void report_error(const std::string& error) {
        log::warn("voice: %s", error.c_str());
        if (options.on_error) {
            options.on_error(error);
        }
    }
};

VoicePeer::VoicePeer(VoicePeerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

VoicePeer::~VoicePeer() {
    close();
}

bool VoicePeer::create_offer(std::string* sdp, std::string* error) {
    if (!sdp) {
        if (error) {
            *error = "no SDP output";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->pc) {
            if (error) {
                *error = "peer connection already exists";
            }
            return false;
        }
    }
    if (!impl_->create_peer(error)) {
        return false;
    }
    std::shared_ptr<rtc::PeerConnection> peer;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        peer = impl_->pc;
    }
    if (!peer) {
        if (error) {
            *error = "peer connection was not created";
        }
        return false;
    }

    try {
        auto audio = rtc::Description::Audio("audio", rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(kOpusPayloadType);
        audio.addSSRC(impl_->ssrc, "puck-kiosk", "puck-voice", "puck-voice");
        auto track = peer->addTrack(audio);
        impl_->setup_track(track);

        auto channel = peer->createDataChannel(kDataChannelLabel);
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->data_channel = channel;
        }
        impl_->setup_data_channel(channel);

        peer->setLocalDescription(rtc::Description::Type::Offer);
        if (!impl_->wait_for_gathering(peer, error)) {
            return false;
        }
        auto description = peer->localDescription();
        if (!description) {
            if (error) {
                *error = "peer produced no local offer";
            }
            return false;
        }
        *sdp = description->generateSdp();
        log::info("voice: offer ready (%zu bytes, %zu candidates)",
                  sdp->size(), count_candidates(*sdp));
        return true;
    } catch (const std::exception& exception) {
        if (error) {
            *error = std::string("cannot create offer: ") + exception.what();
        }
        return false;
    }
}

bool VoicePeer::accept_offer(const std::string& offer_sdp, std::string* answer_sdp, std::string* error) {
    if (!answer_sdp) {
        if (error) {
            *error = "no SDP output";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->pc) {
            if (error) {
                *error = "peer connection already exists";
            }
            return false;
        }
    }
    if (!impl_->create_peer(error)) {
        return false;
    }
    std::shared_ptr<rtc::PeerConnection> peer;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        peer = impl_->pc;
    }
    if (!peer) {
        if (error) {
            *error = "peer connection was not created";
        }
        return false;
    }

    try {
        peer->setRemoteDescription(rtc::Description(offer_sdp, "offer"));
        peer->setLocalDescription(rtc::Description::Type::Answer);
        if (!impl_->wait_for_gathering(peer, error)) {
            return false;
        }
        auto description = peer->localDescription();
        if (!description) {
            if (error) {
                *error = "peer produced no local answer";
            }
            return false;
        }
        *answer_sdp = description->generateSdp();
        log::info("voice: answer ready (%zu bytes, %zu candidates)",
                  answer_sdp->size(), count_candidates(*answer_sdp));
        return true;
    } catch (const std::exception& exception) {
        if (error) {
            *error = std::string("cannot accept offer: ") + exception.what();
        }
        return false;
    }
}

bool VoicePeer::accept_answer(const std::string& answer_sdp, std::string* error) {
    try {
        std::shared_ptr<rtc::PeerConnection> peer;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            peer = impl_->pc;
        }
        if (!peer) {
            if (error) {
                *error = "peer connection was not created";
            }
            return false;
        }
        peer->setRemoteDescription(rtc::Description(answer_sdp, "answer"));
        return true;
    } catch (const std::exception& exception) {
        if (error) {
            *error = std::string("cannot accept answer: ") + exception.what();
        }
        return false;
    }
}

bool VoicePeer::send_audio(const std::uint8_t* payload, std::size_t size,
                           std::uint32_t rtp_timestamp) {
    if (!payload || size == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->send_mutex);
    ++impl_->send_attempts;
    const std::uint16_t sequence = impl_->sequence++;
    std::vector<std::uint8_t> packet =
        build_rtp(kOpusPayloadType, sequence, rtp_timestamp, impl_->ssrc, payload, size);
    if (impl_->options.test_drop_every > 0 &&
        impl_->send_attempts % static_cast<std::uint64_t>(impl_->options.test_drop_every) == 0) {
        // The sequence number is consumed so the peer sees a real gap, like a
        // packet lost in the network.
        ++impl_->dropped_packets;
        return false;
    }

    if (impl_->options.test_reorder_every > 0 &&
        impl_->send_attempts % static_cast<std::uint64_t>(impl_->options.test_reorder_every) == 0) {
        impl_->pending_reorder = std::move(packet);
        return true;
    }

    bool sent = impl_->send_packet_locked(packet, sequence);
    if (impl_->pending_reorder) {
        const std::uint16_t pending_sequence = static_cast<std::uint16_t>(sequence - 1);
        sent = impl_->send_packet_locked(*impl_->pending_reorder, pending_sequence) || sent;
        impl_->pending_reorder.reset();
    }
    return sent;
}

bool VoicePeer::send_nack(const std::vector<std::uint16_t>& sequence_numbers) {
    if (sequence_numbers.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> send_lock(impl_->send_mutex);
    std::shared_ptr<rtc::Track> output;
    std::uint32_t media_ssrc = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        output = impl_->track;
        media_ssrc = impl_->remote_ssrc.load();
    }
    if (!output || !output->isOpen() || media_ssrc == 0) {
        return false;
    }

    RtcpNack nack;
    nack.sender_ssrc = impl_->ssrc;
    nack.media_ssrc = media_ssrc;
    nack.sequence_numbers = sequence_numbers;
    const std::vector<std::uint8_t> bytes = build_rtcp_nack(nack);
    rtc::binary binary(bytes.size());
    std::memcpy(binary.data(), bytes.data(), bytes.size());
    try {
        return output->send(binary);
    } catch (const std::exception& exception) {
        impl_->report_error(std::string("NACK send failed: ") + exception.what());
        return false;
    }
}

bool VoicePeer::send_data(const std::string& text) {
    std::shared_ptr<rtc::DataChannel> channel;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        channel = impl_->data_channel;
    }
    if (!channel || !channel->isOpen()) {
        return false;
    }
    try {
        return channel->send(text);
    } catch (const std::exception& exception) {
        impl_->report_error(std::string("data channel send failed: ") + exception.what());
        return false;
    }
}

PeerState VoicePeer::state() const {
    return impl_->state.load();
}

bool VoicePeer::connected() const {
    return impl_->connected.load();
}

void VoicePeer::close() {
    std::shared_ptr<rtc::PeerConnection> peer;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        peer = impl_->pc;
        impl_->pc.reset();
        impl_->track.reset();
        impl_->data_channel.reset();
        impl_->gathering_complete = true;
    }
    impl_->gathering_cv.notify_all();
    impl_->state = PeerState::Closed;
    impl_->connected = false;
    if (peer) {
        try {
            peer->close();
        } catch (const std::exception& exception) {
            log::warn("voice: peer close failed: %s", exception.what());
        }
    }
}

std::uint64_t VoicePeer::sent_packets() const {
    return impl_->sent_packets.load();
}

std::uint64_t VoicePeer::received_packets() const {
    return impl_->received_packets.load();
}

std::uint64_t VoicePeer::retransmitted_packets() const {
    return impl_->retransmitted_packets.load();
}

std::uint64_t VoicePeer::dropped_packets() const {
    return impl_->dropped_packets.load();
}

std::uint32_t VoicePeer::local_ssrc() const {
    return impl_->ssrc;
}

}  // namespace pk::voice
