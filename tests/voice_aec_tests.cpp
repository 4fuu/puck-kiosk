// Replayable speakerphone tests for the shipped AEC backends.
//
// The default fixture is two short LibriSpeech recordings in tests/fixtures,
// a sparse room echo path, and a gated near-end talker. It is a file test: it
// does not prove the behaviour of a real speaker, microphone, or car cabin.
// Pass --far and --near with 48 kHz mono WAV recordings to replay the same
// measurements on captured audio, and --wav-dir to keep every input and
// output for listening.
#include "voice/aec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kFrame = 960;  // 20 ms
constexpr double kPi = 3.14159265358979323846;

struct Rng {
    std::uint32_t state;
    explicit Rng(std::uint32_t seed) : state(seed ? seed : 1) {}
    double next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / 16777216.0;
    }
    double bipolar() { return next() * 2.0 - 1.0; }
};

double db(double energy, std::size_t count) {
    return 10.0 * std::log10(energy / static_cast<double>(std::max<std::size_t>(1, count)) + 1e-12);
}

double frame_db(const std::int16_t* samples, std::size_t count) {
    double energy = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double value = samples[i];
        energy += value * value;
    }
    return db(energy, count);
}


std::vector<std::int16_t> room_echo(const std::vector<std::int16_t>& far) {
    const std::size_t n = far.size();
    std::vector<double> echo(n, 0.0);
    Rng rng(4242);
    const int direct = 37 * kRate / 1000;
    // One fixed reflection pattern: a room does not change its impulse
    // response from sample to sample.
    struct Reflection {
        int delay;
        double gain;
    };
    std::vector<Reflection> reflections;
    for (int i = 0; i < 48; ++i) {
        const double delay_ms = 18.0 + 150.0 * rng.next();
        const double sign = rng.next() < 0.5 ? -1.0 : 1.0;
        const double gain =
            0.028 * std::exp(-delay_ms / 55.0) * sign * (0.4 + 0.6 * rng.next());
        reflections.push_back({static_cast<int>(delay_ms * kRate / 1000.0), gain});
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (i >= static_cast<std::size_t>(direct)) {
            echo[i] += 0.95 * static_cast<double>(far[i - static_cast<std::size_t>(direct)]) / 32768.0;
        }
        for (const Reflection& reflection : reflections) {
            if (i < static_cast<std::size_t>(reflection.delay)) {
                continue;
            }
            echo[i] += reflection.gain *
                       static_cast<double>(far[i - static_cast<std::size_t>(reflection.delay)]) /
                       32768.0;
        }
    }
    std::vector<std::int16_t> pcm(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double shaped = std::tanh(echo[i] * 1.25) * 0.9;
        pcm[i] = static_cast<std::int16_t>(std::lround(std::clamp(shaped, -1.0, 1.0) * 32767.0));
    }
    return pcm;
}

std::vector<std::int16_t> mix(const std::vector<std::int16_t>& a,
                              const std::vector<std::int16_t>& b) {
    std::vector<std::int16_t> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[i] = static_cast<std::int16_t>(
            std::clamp(static_cast<int>(a[i]) + static_cast<int>(b[i]), -32768, 32767));
    }
    return out;
}

void gate(std::vector<std::int16_t>* samples, int start_s, int end_s) {
    for (int i = start_s * kRate; i < end_s * kRate && i < static_cast<int>(samples->size()); ++i) {
        const int phase = (i - start_s * kRate) % (2 * kRate);
        if (phase > 3 * kRate / 2) {
            (*samples)[static_cast<std::size_t>(i)] = 0;
        }
    }
}

bool read_wav(const std::string& path, std::vector<std::int16_t>* samples, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "cannot open " + path;
        return false;
    }
    auto read_u32 = [&in]() {
        unsigned char bytes[4] = {};
        in.read(reinterpret_cast<char*>(bytes), 4);
        return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
               (static_cast<std::uint32_t>(bytes[2]) << 16) |
               (static_cast<std::uint32_t>(bytes[3]) << 24);
    };
    auto read_u16 = [&in]() {
        unsigned char bytes[2] = {};
        in.read(reinterpret_cast<char*>(bytes), 2);
        return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
    };
    char riff[4] = {};
    char wave[4] = {};
    in.read(riff, 4);
    read_u32();
    in.read(wave, 4);
    if (std::string(riff, 4) != "RIFF" || std::string(wave, 4) != "WAVE") {
        *error = path + " is not a RIFF/WAVE file";
        return false;
    }
    bool have_format = false;
    std::uint32_t input_rate = kRate;
    while (in) {
        char id[4] = {};
        in.read(id, 4);
        const std::uint32_t size = read_u32();
        const std::string chunk(id, 4);
        if (chunk == "fmt ") {
            const std::uint16_t format = read_u16();
            const std::uint16_t channels = read_u16();
            const std::uint32_t rate = read_u32();
            read_u32();
            read_u16();
            const std::uint16_t bits = read_u16();
            if (format != 1 || channels != 1 || bits != 16 || (rate != 16000 && rate != kRate)) {
                *error = path + " must be mono 16-bit PCM at 16000 or 48000 Hz";
                return false;
            }
            input_rate = rate;
            have_format = true;
            if (size > 16) {
                in.seekg(size - 16, std::ios::cur);
            }
        } else if (chunk == "data") {
            if (!have_format) {
                *error = path + " has data before fmt";
                return false;
            }
            std::vector<std::int16_t> input(size / 2);
            in.read(reinterpret_cast<char*>(input.data()), size);
            if (static_cast<std::uint32_t>(in.gcount()) != size) {
                *error = path + " is truncated";
                return false;
            }
            if (input_rate == kRate) {
                *samples = std::move(input);
            } else {
                const double step = static_cast<double>(input_rate) / kRate;
                const std::size_t output_size =
                    static_cast<std::size_t>(static_cast<double>(input.size()) / step);
                samples->resize(output_size);
                for (std::size_t i = 0; i < output_size; ++i) {
                    const double position = static_cast<double>(i) * step;
                    const std::size_t index = static_cast<std::size_t>(position);
                    const double fraction = position - static_cast<double>(index);
                    const double first = index < input.size() ? input[index] : 0.0;
                    const double second = index + 1 < input.size() ? input[index + 1] : first;
                    (*samples)[i] = static_cast<std::int16_t>(
                        std::lround(first + (second - first) * fraction));
                }
            }
            return !samples->empty();
        } else {
            in.seekg(size + (size & 1u), std::ios::cur);
        }
    }
    *error = path + " has no data chunk";
    return false;
}

void write_wav(const std::string& path, const std::vector<std::int16_t>& samples) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return;
    }
    auto u32 = [&out](std::uint32_t value) {
        char bytes[4] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff),
                         static_cast<char>((value >> 16) & 0xff),
                         static_cast<char>((value >> 24) & 0xff)};
        out.write(bytes, 4);
    };
    auto u16 = [&out](std::uint16_t value) {
        char bytes[2] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff)};
        out.write(bytes, 2);
    };
    const std::uint32_t bytes = static_cast<std::uint32_t>(samples.size() * 2);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16);
    u16(1);
    u16(1);
    u32(kRate);
    u32(kRate * 2);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), bytes);
}

struct Align {
    int lag = 0;
    double corr = 0.0;
};

Align align_signals(const std::vector<std::int16_t>& a, const std::vector<std::int16_t>& b,
                    int max_lag) {
    Align best;
    for (int lag = -max_lag; lag <= max_lag; lag += 4) {
        double ab = 0.0;
        double aa = 0.0;
        double bb = 0.0;
        for (std::size_t i = static_cast<std::size_t>(max_lag);
             i + static_cast<std::size_t>(max_lag) < a.size() && i < 4u * kRate; i += 4) {
            const long long j = static_cast<long long>(i) + lag;
            if (j < 0 || j >= static_cast<long long>(b.size())) {
                continue;
            }
            const double x = a[i];
            const double y = b[static_cast<std::size_t>(j)];
            ab += x * y;
            aa += x * x;
            bb += y * y;
        }
        const double corr = ab / std::sqrt(aa * bb + 1e-12);
        if (corr > best.corr) {
            best.corr = corr;
            best.lag = lag;
        }
    }
    return best;
}

void align_pair(std::vector<std::int16_t>* a, std::vector<std::int16_t>* b, int lag) {
    if (lag > 0) {
        b->erase(b->begin(), b->begin() + lag);
        a->resize(std::min(a->size(), b->size()));
    } else if (lag < 0) {
        a->erase(a->begin(), a->begin() + (-lag));
        b->resize(std::min(a->size(), b->size()));
    }
}

struct Metrics {
    double erle_db = 0.0;
    double user_gain_db = 0.0;
    double user_corr = 0.0;
    double double_gain_db = 0.0;
    double double_corr = 0.0;
    double double_echo_db = 0.0;
    double noise_floor_db = 0.0;
    double quiet_gain_db = 0.0;
    bool have_erle = false;
    bool have_user = false;
    bool have_double = false;
    bool have_noise = false;
    bool have_quiet = false;
};

std::vector<std::int16_t> process(const pk::voice::AecSettings& settings,
                                  const std::vector<std::int16_t>& render,
                                  const std::vector<std::int16_t>& capture) {
    pk::voice::EchoCanceller aec;
    std::string error;
    if (!aec.initialize(kRate, 1, settings, &error)) {
        std::fprintf(stderr, "AEC init failed: %s\n", error.c_str());
        return {};
    }
    std::vector<std::int16_t> output = capture;
    for (std::size_t offset = 0; offset + kFrame <= output.size(); offset += kFrame) {
        aec.process_render(render.data() + offset, kFrame);
        aec.process_capture(output.data() + offset, kFrame);
    }
    return output;
}


void measure_echo(const std::vector<std::int16_t>& echo, const std::vector<std::int16_t>& output,
                  Metrics* metrics) {
    double input = 0.0;
    double residual = 0.0;
    for (std::size_t frame = 6 * kRate / kFrame; frame < echo.size() / kFrame; ++frame) {
        const std::int16_t* e = echo.data() + frame * kFrame;
        if (frame_db(e, kFrame) < -60.0) {
            continue;
        }
        const std::int16_t* o = output.data() + frame * kFrame;
        input += std::pow(10.0, frame_db(e, kFrame) / 10.0);
        residual += std::pow(10.0, frame_db(o, kFrame) / 10.0);
    }
    metrics->erle_db = 10.0 * std::log10((input + 1e-12) / (residual + 1e-12));
    metrics->have_erle = true;
}

void measure_user(const std::vector<std::int16_t>& near, const std::vector<std::int16_t>& output,
                  Metrics* metrics) {
    std::vector<std::int16_t> a(output.begin() + 2 * kRate, output.end());
    std::vector<std::int16_t> b(near.begin() + 2 * kRate, near.end());
    const Align align = align_signals(a, b, 960);
    align_pair(&a, &b, align.lag);
    double input = 0.0;
    double preserved = 0.0;
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (std::size_t frame = 0; frame < b.size() / kFrame; ++frame) {
        const std::int16_t* n = b.data() + frame * kFrame;
        if (frame_db(n, kFrame) < -45.0) {
            continue;
        }
        const std::int16_t* o = a.data() + frame * kFrame;
        for (int i = 0; i < kFrame; ++i) {
            input += static_cast<double>(n[i]) * n[i];
            preserved += static_cast<double>(o[i]) * o[i];
            ab += static_cast<double>(n[i]) * o[i];
            aa += static_cast<double>(n[i]) * n[i];
            bb += static_cast<double>(o[i]) * o[i];
        }
    }
    metrics->user_gain_db = 10.0 * std::log10((preserved + 1e-12) / (input + 1e-12));
    metrics->user_corr = ab / std::sqrt(aa * bb + 1e-12);
    metrics->have_user = true;
}

void measure_double(const std::vector<std::int16_t>& near, const std::vector<std::int16_t>& echo,
                    const std::vector<std::int16_t>& output, Metrics* metrics) {
    std::vector<std::int16_t> a(output.begin() + 2 * kRate, output.end());
    std::vector<std::int16_t> b(near.begin() + 2 * kRate, near.end());
    std::vector<std::int16_t> e(echo.begin() + 2 * kRate, echo.end());
    const Align align = align_signals(a, b, 960);
    align_pair(&a, &b, align.lag);
    if (align.lag < 0) {
        e.erase(e.begin(), e.begin() + (-align.lag));
    }
    e.resize(std::min(e.size(), a.size()));
    double input = 0.0;
    double preserved = 0.0;
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    double echo_energy = 0.0;
    double residual_energy = 0.0;
    for (std::size_t frame = 0; frame < b.size() / kFrame; ++frame) {
        const std::int16_t* n = b.data() + frame * kFrame;
        const std::int16_t* o = a.data() + frame * kFrame;
        const std::int16_t* r = e.data() + frame * kFrame;
        const double near_db = frame_db(n, kFrame);
        if (near_db > -45.0) {
            for (int i = 0; i < kFrame; ++i) {
                input += static_cast<double>(n[i]) * n[i];
                preserved += static_cast<double>(o[i]) * o[i];
                ab += static_cast<double>(n[i]) * o[i];
                aa += static_cast<double>(n[i]) * n[i];
                bb += static_cast<double>(o[i]) * o[i];
            }
        } else if (near_db < -70.0 && frame_db(r, kFrame) > -60.0) {
            for (int i = 0; i < kFrame; ++i) {
                echo_energy += static_cast<double>(r[i]) * r[i];
                residual_energy += static_cast<double>(o[i]) * o[i];
            }
        }
    }
    metrics->double_gain_db = 10.0 * std::log10((preserved + 1e-12) / (input + 1e-12));
    metrics->double_corr = ab / std::sqrt(aa * bb + 1e-12);
    metrics->double_echo_db =
        10.0 * std::log10((echo_energy + 1e-12) / (residual_energy + 1e-12));
    metrics->have_double = true;
}

void measure_noise(const std::vector<std::int16_t>& near, const std::vector<std::int16_t>& noise,
                   const std::vector<std::int16_t>& output, Metrics* metrics) {
    std::vector<std::int16_t> a(output.begin() + 2 * kRate, output.end());
    std::vector<std::int16_t> b(near.begin() + 2 * kRate, near.end());
    std::vector<std::int16_t> n(noise.begin() + 2 * kRate, noise.end());
    const Align align = align_signals(a, b, 960);
    align_pair(&a, &b, align.lag);
    if (align.lag < 0) {
        n.erase(n.begin(), n.begin() + (-align.lag));
    }
    n.resize(std::min(n.size(), a.size()));
    double noise_in = 0.0;
    double noise_out = 0.0;
    double near_in = 0.0;
    double near_out = 0.0;
    for (std::size_t frame = 0; frame < b.size() / kFrame; ++frame) {
        const std::int16_t* near_frame = b.data() + frame * kFrame;
        const std::int16_t* out_frame = a.data() + frame * kFrame;
        const std::int16_t* noise_frame = n.data() + frame * kFrame;
        if (frame_db(near_frame, kFrame) < -70.0) {
            for (int i = 0; i < kFrame; ++i) {
                noise_in += static_cast<double>(noise_frame[i]) * noise_frame[i];
                noise_out += static_cast<double>(out_frame[i]) * out_frame[i];
            }
        } else if (frame_db(near_frame, kFrame) > -45.0) {
            for (int i = 0; i < kFrame; ++i) {
                near_in += static_cast<double>(near_frame[i]) * near_frame[i];
                near_out += static_cast<double>(out_frame[i]) * out_frame[i];
            }
        }
    }
    metrics->noise_floor_db =
        10.0 * std::log10((noise_out + 1e-12) / (noise_in + 1e-12));
    metrics->user_gain_db = 10.0 * std::log10((near_out + 1e-12) / (near_in + 1e-12));
    metrics->have_noise = true;
}

struct Fixture {
    std::vector<std::int16_t> far;
    std::vector<std::int16_t> near;
    std::vector<std::int16_t> near_double;
    std::vector<std::int16_t> echo;
    std::vector<std::int16_t> noise;
    int seconds = 12;
};

bool make_fixture(int seconds, const std::string& far_path, const std::string& near_path,
                  Fixture* fixture, std::string* error) {
    fixture->seconds = seconds;
    const std::size_t total = static_cast<std::size_t>(seconds) * kRate;
    std::vector<std::int16_t> far;
    std::vector<std::int16_t> near;
    if (!read_wav(far_path, &far, error) || !read_wav(near_path, &near, error)) {
        return false;
    }
    fixture->far.assign(total, 0);
    fixture->near.assign(total, 0);
    std::copy(far.begin(),
              far.begin() + static_cast<std::ptrdiff_t>(std::min(total, far.size())),
              fixture->far.begin());
    std::copy(near.begin(),
              near.begin() + static_cast<std::ptrdiff_t>(std::min(total, near.size())),
              fixture->near.begin());
    fixture->near_double = fixture->near;
    gate(&fixture->near_double, 2, seconds);
    fixture->echo = room_echo(fixture->far);
    fixture->noise.assign(total, 0);
    Rng rng(9001);
    for (std::size_t i = 0; i < total; ++i) {
        fixture->noise[i] = static_cast<std::int16_t>(std::lround(rng.bipolar() * 0.004 * 32767.0));
    }
    return true;
}

std::vector<std::int16_t> with_noise(const std::vector<std::int16_t>& near,
                                     const std::vector<std::int16_t>& noise) {
    return mix(near, noise);
}

struct Variant {
    const char* name;
    bool echo;
    bool ns;
    bool agc;
};

void print_row(const char* backend, const char* variant, const Metrics& metrics) {
    std::printf("%-24s %-12s", backend, variant);
    if (metrics.have_erle) {
        std::printf(" ERLE %6.1f", metrics.erle_db);
    } else {
        std::printf(" ERLE %6s", "-");
    }
    if (metrics.have_user) {
        std::printf("  user %+6.1f dB c %5.2f", metrics.user_gain_db, metrics.user_corr);
    } else {
        std::printf("  user %6s", "-");
    }
    if (metrics.have_double) {
        std::printf("  DT %+6.1f dB c %5.2f echo %5.1f", metrics.double_gain_db,
                    metrics.double_corr, metrics.double_echo_db);
    } else {
        std::printf("  DT %s", "-");
    }
    if (metrics.have_noise) {
        std::printf("  noise %+6.1f dB", metrics.noise_floor_db);
    }
    if (metrics.have_quiet) {
        std::printf("  quiet %+6.1f dB", metrics.quiet_gain_db);
    }
    std::printf("\n");
}


bool check_backend(pk::voice::AecBackend backend, const char* expected, bool* failed) {
    pk::voice::AecSettings settings;
    settings.backend = backend;
    pk::voice::EchoCanceller aec;
    std::string error;
    if (!aec.initialize(kRate, 1, settings, &error)) {
        std::fprintf(stderr, "FAIL %s: initialize: %s\n", expected, error.c_str());
        *failed = true;
        return false;
    }
    const pk::voice::AecStatus& status = aec.status();
    if (status.backend != expected || !aec.enabled() || !status.echo_cancellation ||
        !status.noise_suppression || status.automatic_gain) {
        std::fprintf(stderr,
                     "FAIL %s: reported backend=%s enabled=%d echo=%d ns=%d agc=%d\n",
                     expected, status.backend.c_str(), aec.enabled() ? 1 : 0,
                     status.echo_cancellation ? 1 : 0, status.noise_suppression ? 1 : 0,
                     status.automatic_gain ? 1 : 0);
        *failed = true;
        return false;
    }
    return true;
}

bool check_bypass(bool* failed) {
    pk::voice::AecSettings settings;
    settings.echo_cancellation = false;
    settings.noise_suppression = false;
    settings.automatic_gain = false;
    pk::voice::EchoCanceller aec;
    std::string error;
    if (!aec.initialize(kRate, 1, settings, &error)) {
        std::fprintf(stderr, "FAIL bypass: initialize: %s\n", error.c_str());
        *failed = true;
        return false;
    }
    if (aec.enabled() || aec.status().backend != "none") {
        std::fprintf(stderr, "FAIL bypass: enabled=%d backend=%s\n", aec.enabled() ? 1 : 0,
                     aec.status().backend.c_str());
        *failed = true;
        return false;
    }
    return true;
}

std::vector<std::int16_t> scaled(const std::vector<std::int16_t>& samples, double gain) {
    std::vector<std::int16_t> out(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        out[i] = static_cast<std::int16_t>(std::lround(static_cast<double>(samples[i]) * gain));
    }
    return out;
}

bool run_backend(pk::voice::AecBackend backend, const Fixture& fixture, bool write_wavs,
                 const std::string& wav_dir, bool* failed) {
    const pk::voice::AecCapabilities capabilities = pk::voice::aec_capabilities();
    if (backend == pk::voice::AecBackend::WebRtc && !capabilities.webrtc) {
        return true;
    }
    if (backend == pk::voice::AecBackend::Portable && !capabilities.portable) {
        return true;
    }
    const std::string backend_name =
        backend == pk::voice::AecBackend::WebRtc ? "webrtc-audio-processing" : "speexdsp";
    check_backend(backend, backend_name.c_str(), failed);
    double smoke_erle = 0.0;
    std::string smoke_error;
    if (pk::voice::run_aec_smoke_test(5, backend, &smoke_erle, &smoke_error)) {
        std::printf("\nbackend: %s (smoke ERLE %.1f dB)\n", backend_name.c_str(), smoke_erle);
    } else {
        std::fprintf(stderr, "FAIL %s: smoke: %s\n", backend_name.c_str(), smoke_error.c_str());
        *failed = true;
        std::printf("\nbackend: %s (smoke failed)\n", backend_name.c_str());
    }
    std::printf("%-24s %-12s %s\n", "scenario", "variant", "measurements");

    const Variant variants[] = {
        {"off", false, false, false},
        {"aec", true, false, false},
        {"aec+ns", true, true, false},
        {"aec+ns+agc", true, true, true},
        {"ns", false, true, false},
    };
    const std::vector<std::int16_t> silence(fixture.far.size(), 0);

    for (const Variant& variant : variants) {
        pk::voice::AecSettings settings;
        settings.backend = backend;
        settings.echo_cancellation = variant.echo;
        settings.noise_suppression = variant.ns;
        settings.automatic_gain = variant.agc;
        settings.tail_ms = 200;

        const std::vector<std::int16_t> echo_out =
            process(settings, fixture.far, fixture.echo);
        const std::vector<std::int16_t> user_out =
            process(settings, silence, fixture.near);
        const std::vector<std::int16_t> double_out =
            process(settings, fixture.far, mix(fixture.echo, fixture.near_double));
        const std::vector<std::int16_t> noise_out =
            process(settings, silence, with_noise(fixture.near_double, fixture.noise));
        const std::vector<std::int16_t> quiet_near = scaled(fixture.near, 0.25);
        const std::vector<std::int16_t> quiet_out = process(settings, silence, quiet_near);

        if (echo_out.empty() || user_out.empty() || double_out.empty() || noise_out.empty() ||
            quiet_out.empty()) {
            std::fprintf(stderr, "AEC processing failed for %s/%s\n", backend_name.c_str(),
                         variant.name);
            *failed = true;
            return false;
        }
        Metrics echo_metrics;
        Metrics user_metrics;
        Metrics double_metrics;
        Metrics noise_metrics;
        measure_echo(fixture.echo, echo_out, &echo_metrics);
        measure_user(fixture.near, user_out, &user_metrics);
        measure_double(fixture.near_double, fixture.echo, double_out, &double_metrics);
        measure_noise(fixture.near_double, fixture.noise, noise_out, &noise_metrics);
        Metrics quiet_metrics;
        measure_user(quiet_near, quiet_out, &quiet_metrics);
        echo_metrics.have_user = user_metrics.have_user;
        echo_metrics.user_gain_db = user_metrics.user_gain_db;
        echo_metrics.user_corr = user_metrics.user_corr;
        echo_metrics.have_double = double_metrics.have_double;
        echo_metrics.double_gain_db = double_metrics.double_gain_db;
        echo_metrics.double_corr = double_metrics.double_corr;
        echo_metrics.double_echo_db = double_metrics.double_echo_db;
        echo_metrics.have_noise = noise_metrics.have_noise;
        echo_metrics.noise_floor_db = noise_metrics.noise_floor_db;
        echo_metrics.have_quiet = quiet_metrics.have_user;
        echo_metrics.quiet_gain_db = quiet_metrics.user_gain_db;
        print_row(backend_name.c_str(), variant.name, echo_metrics);

        if (write_wavs) {
            std::filesystem::create_directories(wav_dir);
            const std::string prefix = wav_dir + "/" + backend_name + "-" + variant.name;
            write_wav(prefix + "-echo.wav", echo_out);
            write_wav(prefix + "-user.wav", user_out);
            write_wav(prefix + "-double.wav", double_out);
            write_wav(prefix + "-noise.wav", noise_out);
        }

        if (!variant.echo && !variant.ns && !variant.agc) {
            if (echo_metrics.erle_db > 2.0 || std::fabs(user_metrics.user_gain_db) > 0.5 ||
                user_metrics.user_corr < 0.99 || std::fabs(double_metrics.double_echo_db) > 2.0) {
                std::fprintf(stderr,
                             "FAIL %s/off: bypass changed the signal (ERLE %.1f, near %+.1f dB, "
                             "corr %.2f, echo %.1f dB)\n",
                             backend_name.c_str(), echo_metrics.erle_db,
                             user_metrics.user_gain_db, user_metrics.user_corr,
                             double_metrics.double_echo_db);
                *failed = true;
            }
        }
        if (backend == pk::voice::AecBackend::Portable && variant.agc) {
            // Guard the AGC target type: passing 8000.0f where SpeexDSP reads
            // spx_int32_t makes it clamp to 32768, which showed up as a large
            // quiet-speech boost (about +14 dB) instead of the correct +2 dB.
            if (quiet_metrics.user_gain_db > 8.0) {
                std::fprintf(stderr,
                             "FAIL speexdsp/aec+ns+agc: quiet gain %+.1f dB suggests the AGC "
                             "target was not written as int32\n",
                             quiet_metrics.user_gain_db);
                *failed = true;
            }
        }
        if (variant.echo && variant.ns && !variant.agc) {
            if (echo_metrics.erle_db < 15.0) {
                std::fprintf(stderr, "FAIL %s/%s: ERLE %.1f dB < 15 dB\n", backend_name.c_str(),
                             variant.name, echo_metrics.erle_db);
                *failed = true;
            }
            if (user_metrics.user_gain_db < -2.5 || user_metrics.user_gain_db > 2.5 ||
                user_metrics.user_corr < 0.80) {
                std::fprintf(stderr,
                             "FAIL %s/%s: near speech gain %+.1f dB corr %.2f\n",
                             backend_name.c_str(), variant.name, user_metrics.user_gain_db,
                             user_metrics.user_corr);
                *failed = true;
            }
            if (double_metrics.double_echo_db < 10.0 ||
                double_metrics.double_gain_db < -3.5 ||
                double_metrics.double_gain_db > 3.5 || double_metrics.double_corr < 0.75) {
                std::fprintf(stderr,
                             "FAIL %s/%s: double talk gain %+.1f dB corr %.2f echo %.1f dB\n",
                             backend_name.c_str(), variant.name, double_metrics.double_gain_db,
                             double_metrics.double_corr, double_metrics.double_echo_db);
                *failed = true;
            }
            if (noise_metrics.noise_floor_db > -4.0 ||
                noise_metrics.user_gain_db < -3.5 || noise_metrics.user_gain_db > 3.5) {
                std::fprintf(stderr, "FAIL %s/%s: noise floor %+.1f dB, near %+.1f dB\n",
                             backend_name.c_str(), variant.name, noise_metrics.noise_floor_db,
                             noise_metrics.user_gain_db);
                *failed = true;
            }
        }
    }

    if (write_wavs) {
        std::filesystem::create_directories(wav_dir);
        write_wav(wav_dir + "/fixture-far.wav", fixture.far);
        write_wav(wav_dir + "/fixture-near.wav", fixture.near);
        write_wav(wav_dir + "/fixture-near-double.wav", fixture.near_double);
        write_wav(wav_dir + "/fixture-echo.wav", fixture.echo);
        write_wav(wav_dir + "/fixture-noise.wav", fixture.noise);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    bool write_wavs = false;
    std::string wav_dir = "build/aec-wav";
    std::string far_path;
    std::string near_path;
    std::string backend_arg = "auto";
    int seconds = 12;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--wav-dir" && i + 1 < argc) {
            write_wavs = true;
            wav_dir = argv[++i];
        } else if (arg == "--far" && i + 1 < argc) {
            far_path = argv[++i];
        } else if (arg == "--near" && i + 1 < argc) {
            near_path = argv[++i];
        } else if (arg == "--backend" && i + 1 < argc) {
            backend_arg = argv[++i];
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::max(8, std::atoi(argv[++i]));
        } else if (arg == "--help") {
            std::printf("usage: voice-aec-tests [--backend auto|apm|portable] "
                        "[--seconds N] [--far FILE.wav] [--near FILE.wav] [--wav-dir DIR]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    if (backend_arg != "auto" && backend_arg != "apm" && backend_arg != "portable") {
        std::fprintf(stderr, "--backend must be auto, apm, or portable\n");
        return 2;
    }

    if (far_path.empty() && near_path.empty()) {
        far_path = "tests/fixtures/speech-far-16k.wav";
        near_path = "tests/fixtures/speech-near-16k.wav";
    }
    if (far_path.empty() != near_path.empty()) {
        std::fprintf(stderr, "--far and --near must be supplied together\n");
        return 2;
    }
    Fixture fixture;
    std::string error;
    if (!make_fixture(seconds, far_path, near_path, &fixture, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    std::printf("recorded fixture, %d s, 48 kHz mono; file test, not a hardware measurement\n",
                seconds);
    const pk::voice::AecCapabilities capabilities = pk::voice::aec_capabilities();
    std::printf("backends built: webrtc=%d portable=%d\n", capabilities.webrtc ? 1 : 0,
                capabilities.portable ? 1 : 0);
    if (!capabilities.built) {
        std::fprintf(stderr, "no AEC backend is compiled into this test\n");
        return 2;
    }
    if ((backend_arg == "apm" && !capabilities.webrtc) ||
        (backend_arg == "portable" && !capabilities.portable)) {
        std::fprintf(stderr, "requested backend is not compiled into this test\n");
        return 2;
    }

    bool failed = false;
    // Capability/status contract: the backend reports what it actually runs,
    // and Off is a true bypass rather than a silent processor.
    check_bypass(&failed);
    if (backend_arg == "auto" || backend_arg == "apm") {
        run_backend(pk::voice::AecBackend::WebRtc, fixture, write_wavs, wav_dir, &failed);
    }
    if (backend_arg == "auto" || backend_arg == "portable") {
        run_backend(pk::voice::AecBackend::Portable, fixture, write_wavs, wav_dir, &failed);
    }
    if (failed) {
        std::printf("\nresult: FAIL\n");
        return 1;
    }
    std::printf("\nresult: PASS (file test only; not a hardware measurement)\n");
    return 0;
}
