// JACK capture - the JACK twin of alsa_capture.h / wasapi_capture.h.
//
// Same contract: `open` picks a source by index, name substring or the
// default, `start(sink)` delivers MONO float frames plus the time the first
// sample was captured, and every measurement the console prints exists here
// too. What differs is what a "device" is. JACK has ports, one per channel, so
// the list is every audio source port in the graph, a pick wires it to this
// client's `gdi2m:in`, and the default is to wire nothing and leave `in` for a
// DAW send or a patchbay.
//
// Capture runs at the graph's rate and period; the engine resamples to the
// model's, so nothing here needs to match 24 kHz.
#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "jack_client.h"
#include "live_util.h"

namespace gdi2m {

struct CaptureDevice {
    std::string name;
    std::string id;       // the JACK port to wire in; empty = leave `in` unwired
    bool isDefault = false;
};

class JackCapture {
public:
    // mono samples, count, and the capture time of mono[0] in hns.
    using Sink = std::function<void(const float*, size_t, int64_t)>;

    ~JackCapture() { stop(); close(); }

    static std::string hrText(int e) { return std::to_string(e); }

    // Entry 0 is "wire nothing", and the default. Then every audio source port
    // except sink monitors, which are a patchbay job: listing them doubles the
    // list and makes every device name ambiguous as a substring.
    static bool list(std::vector<CaptureDevice>& out, std::string& err) {
        JackClient& jc = JackClient::get();
        if (!jc.open(err)) return false;
        out.push_back({"(nothing - wire a source to " + jc.fullName("in") + " yourself)", "", true});
        for (auto& p : jc.ports(JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput)) {
            if (p.find(":monitor_") != std::string::npos) continue;
            out.push_back({p, p, false});
        }
        return true;
    }

    struct ExclusiveFormat {
        int rate = 0, channels = 0, bits = 0, valid = 0;
        bool isFloat = false;
    };
    struct DeviceProbe {
        std::string name, error;
        bool ok = false;
        int mixRate = 0, mixChannels = 0, mixBits = 0;
        bool mixFloat = false;
        bool haveClient3 = false;
        UINT32 shDefault = 0, shFundamental = 0, shMin = 0, shMax = 0;   // frames
        double engineDefaultMs = 0.0, deviceMinMs = 0.0;
        std::vector<ExclusiveFormat> exclusive;
    };

    // One rate and one period for the whole graph, so every port probes alike.
    static bool probe(const CaptureDevice& d, DeviceProbe& p) {
        p.name = d.name;
        JackClient& jc = JackClient::get();
        if (!jc.open(p.error)) return false;
        p.mixRate = jc.rate();
        p.mixChannels = 1;
        p.mixBits = 32;
        p.mixFloat = true;
        p.shDefault = p.shMin = jc.period();
        p.engineDefaultMs = p.mixRate ? 1000.0 * jc.period() / p.mixRate : 0.0;
        p.deviceMinMs = p.engineDefaultMs;
        p.ok = true;
        return true;
    }

    // spec: null/empty = wire nothing, all digits = index into list(), else
    // case-insensitive substring of the port name (same convention as the
    // MIDI port). inChannel and periodFrames have no meaning here: a JACK port
    // IS one channel, and the period belongs to the graph - PIPEWIRE_LATENCY
    // (default 128/48000, see jack_client.h) is how to ask for another.
    bool open(const char* spec, int /*inChannel*/, int /*periodFrames*/, bool /*wantExclusive*/,
              std::string& err) {
        std::vector<CaptureDevice> devs;
        if (!list(devs, err)) return false;

        int pick = 0;
        if (spec && *spec) {
            const std::string s = spec;
            if (s.find_first_not_of("0123456789") == std::string::npos) {
                pick = std::atoi(spec);
                if (pick < 0 || pick >= static_cast<int>(devs.size())) {
                    err = "device index out of range"; return false;
                }
            } else {
                pick = -1;
                const std::string want = asciiLower(s);
                for (size_t i = 1; i < devs.size(); ++i) {
                    if (asciiLower(devs[i].name).find(want) != std::string::npos) {
                        if (pick >= 0) { err = "'" + s + "' matches more than one port"; return false; }
                        pick = static_cast<int>(i);
                    }
                }
                if (pick < 0) { err = "no capture port matching '" + s + "'"; return false; }
            }
        }
        source_ = devs[static_cast<size_t>(pick)].id;

        JackClient& jc = JackClient::get();
        port_ = jc.port("in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
        if (!port_) { err = "cannot register " + jc.fullName("in"); return false; }
        if (!source_.empty()) {
            const int rc = jack_connect(jc.handle(), source_.c_str(), jc.fullName("in").c_str());
            if (rc != 0 && rc != EEXIST) {
                err = "cannot wire " + source_ + " to " + jc.fullName("in");
                return false;
            }
        }
        name_ = jc.fullName("in") + (source_.empty() ? "  (unwired)" : "  <-  " + source_);
        rate_ = jc.rate();
        periodFrames_ = jc.period();
        fmtText_ = std::to_string(rate_) + " Hz, 1 ch, 32-bit float (JACK)";
        xrunsAtOpen_ = jc.xruns();
        return true;
    }

    bool start(Sink sink, std::string& err) {
        if (!port_) { err = "not open"; return false; }
        sink_ = std::move(sink);
        handler_ = {&JackCapture::onCycle, this};
        JackClient::get().attachCapture(&handler_);
        started_ = true;
        return true;
    }

    void stop() {
        if (!started_) return;
        JackClient::get().detachCapture();
        started_ = false;
    }

    // The port stays registered (see JackClient::port); only this session's
    // own wire comes off, so a hand-made one survives a restart.
    void close() {
        if (!port_) return;
        JackClient& jc = JackClient::get();
        if (!source_.empty())
            jack_disconnect(jc.handle(), source_.c_str(), jc.fullName("in").c_str());
        port_ = nullptr;
    }

    const std::string& deviceName() const { return name_; }
    const std::string& formatText() const { return fmtText_; }
    int rate() const { return rate_; }
    int channels() const { return 1; }
    UINT32 bufferFrames() const { return periodFrames_; }
    UINT32 periodFrames() const { return periodFrames_; }
    UINT32 engineMinPeriod() const { return periodFrames_; }
    UINT32 engineDefaultPeriod() const { return periodFrames_; }
    bool usedClient3() const { return false; }
    bool exclusive() const { return false; }
    // Why exclusive was not used, when it was asked for. Empty = it was.
    const std::string& exclusiveNote() const { return exclusiveNote_; }
    uint64_t discontinuities() const { return JackClient::get().xruns() - xrunsAtOpen_; }
    uint64_t framesCaptured() const { return framesCaptured_.load(); }

    double deliveryLagMs() const {
        const uint64_t p = deliveryPackets_.load();
        return p ? hnsToMs(deliveryLagSum_.load()) / static_cast<double>(p) : 0.0;
    }
    double packetMs() const {
        const uint64_t p = deliveryPackets_.load();
        return p ? 1000.0 * static_cast<double>(deliveryFrames_.load()) /
                       (static_cast<double>(p) * rate_)
                 : 0.0;
    }
    LevelMeter meter;

private:
    static void onCycle(void* ctx, jack_nframes_t frames, int64_t cycleStartHns) {
        static_cast<JackCapture*>(ctx)->cycle(frames, cycleStartHns);
    }

    void cycle(jack_nframes_t frames, int64_t cycleStartHns) {
        if (!frames) return;
        const float* in = static_cast<const float*>(jack_port_get_buffer(port_, frames));
        float peak = 0.0f;
        double sumsq = 0.0;
        for (jack_nframes_t i = 0; i < frames; ++i) {
            const float a = std::fabs(in[i]);
            if (a > peak) peak = a;
            sumsq += static_cast<double>(in[i]) * in[i];
        }
        meter.bump(meter.peak, peak);
        meter.bump(meter.sessionPeak, peak);
        meter.bump(meter.chPeak[0], peak);
        meter.rms.store(static_cast<float>(std::sqrt(sumsq / static_cast<double>(frames))),
                        std::memory_order_relaxed);
        if (peak >= 0.999f) meter.clipped.fetch_add(1, std::memory_order_relaxed);

        // A cycle starts when its period has been captured, so the first
        // sample is one period older than the cycle. What the interface and
        // USB add ahead of that is not visible from here.
        const int64_t stamp =
            cycleStartHns - static_cast<int64_t>(frames) * 10000000 / std::max(1, rate_);
        deliveryLagSum_.fetch_add(nowHns() - stamp, std::memory_order_relaxed);
        deliveryPackets_.fetch_add(1, std::memory_order_relaxed);
        deliveryFrames_.fetch_add(frames, std::memory_order_relaxed);
        framesCaptured_.fetch_add(frames, std::memory_order_relaxed);
        sink_(in, frames, stamp);
    }

    jack_port_t* port_ = nullptr;
    JackClient::Handler handler_;
    bool started_ = false;
    Sink sink_;
    std::string name_, fmtText_, source_, exclusiveNote_;
    int rate_ = 0;
    UINT32 periodFrames_ = 0;
    uint64_t xrunsAtOpen_ = 0;
    std::atomic<uint64_t> framesCaptured_{0};
    std::atomic<int64_t> deliveryLagSum_{0};
    std::atomic<uint64_t> deliveryPackets_{0}, deliveryFrames_{0};
};

}  // namespace gdi2m
