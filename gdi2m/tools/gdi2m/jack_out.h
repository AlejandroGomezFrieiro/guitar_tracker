// JACK sine monitor - the JACK twin of alsa_out.h / sine_out.h.
//
// Same job and the same command ring: the inference thread pushes
// note-on/off and the audio thread turns them into sines, so pluck-to-sound
// can be judged with no synth in the path. The voices are rendered inside the
// client's process callback into `gdi2m:monitor`, which adds no buffer of its
// own - what is left is the period of whatever sink it is wired to.
//
// A "device" here is a client that has audio playback ports; picking one
// wires `monitor` to every one of them, so mono lands on both sides.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "jack_client.h"
#include "live_util.h"

namespace gdi2m {

struct RenderDevice {
    std::string name;
    std::string id;       // the client whose playback ports get wired
    bool isDefault = false;
};

class JackSineOut {
public:
    static constexpr int kPitches = 128;
    static constexpr size_t kCmdCap = 512;

    ~JackSineOut() { stop(); close(); }

    // One entry per client with playback ports, in the order JACK reports
    // them; PipeWire puts the default sink first.
    static bool list(std::vector<RenderDevice>& out, std::string& err) {
        JackClient& jc = JackClient::get();
        if (!jc.open(err)) return false;
        for (const auto& p : jc.ports(JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsPhysical)) {
            const std::string client = p.substr(0, p.rfind(':'));
            const bool seen = std::any_of(out.begin(), out.end(),
                                          [&](const RenderDevice& d) { return d.id == client; });
            if (!seen) out.push_back({client, client, out.empty()});
        }
        return true;
    }

    // periodFrames has no meaning here - see JackCapture::open.
    bool open(const char* spec, int /*periodFrames*/, std::string& err) {
        std::vector<RenderDevice> devs;
        if (!list(devs, err)) return false;
        if (devs.empty()) { err = "no playback ports"; return false; }

        int want = -1;
        const std::string s = spec ? spec : "";
        if (s.empty()) {
            want = 0;
        } else if (s.find_first_not_of("0123456789") == std::string::npos) {
            want = std::atoi(s.c_str());
            if (want < 0 || want >= static_cast<int>(devs.size())) {
                err = "render device index out of range";
                return false;
            }
        } else {
            const std::string low = asciiLower(s);
            for (size_t i = 0; i < devs.size() && want < 0; ++i)
                if (asciiLower(devs[i].name).find(low) != std::string::npos)
                    want = static_cast<int>(i);
        }
        if (want < 0) {
            err = "no render device matching '" + s + "'";
            return false;
        }
        name_ = devs[static_cast<size_t>(want)].name;

        JackClient& jc = JackClient::get();
        port_ = jc.port("monitor", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput);
        if (!port_) { err = "cannot register " + jc.fullName("monitor"); return false; }
        wired_.clear();
        for (const auto& p : jc.ports(JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsPhysical)) {
            if (p.substr(0, p.rfind(':')) != name_) continue;
            const int rc = jack_connect(jc.handle(), jc.fullName("monitor").c_str(), p.c_str());
            if (rc == 0 || rc == EEXIST) wired_.push_back(p);
            else {
                err = "cannot wire " + jc.fullName("monitor") + " to " + p;
                close();
                return false;
            }
        }
        rate_ = jc.rate();
        periodFrames_ = jc.period();
        reset();
        return true;
    }

    bool start(std::string& err) {
        if (!port_) { err = "not open"; return false; }
        handler_ = {&JackSineOut::onCycle, this};
        JackClient::get().attachRender(&handler_);
        started_ = true;
        return true;
    }

    void stop() {
        if (!started_) return;
        JackClient::get().detachRender();
        started_ = false;
    }

    // Unlike `in` and `midi_out` the port goes away with the session: nothing
    // renders into it once the handler is detached, and an output port nobody
    // writes plays whatever was left in its buffer.
    void close() {
        if (!port_) return;
        jack_port_unregister(JackClient::get().handle(), port_);
        wired_.clear();
        port_ = nullptr;
    }

    // ---- the inference thread's side -------------------------------------
    // Lock-free SPSC; a full ring DROPS rather than blocks (see sine_out.h).
    void noteOn(int pitch) { push(pitch, 1); }
    void noteOff(int pitch) { push(pitch, 0); }
    void allOff() { push(-1, 0); }

    bool valid() const { return port_ != nullptr; }
    const std::string& name() const { return name_; }
    UINT32 rate() const { return static_cast<UINT32>(rate_); }
    UINT32 channels() const { return 1; }
    UINT32 periodFrames() const { return periodFrames_; }
    UINT32 bufferFrames() const { return periodFrames_; }
    bool usedClient3() const { return false; }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    // The graph's xruns are counted once, on the capture side.
    uint64_t underruns() const { return 0; }
    double bufferMs() const {
        return rate_ ? 1000.0 * periodFrames_ / rate_ : 0.0;
    }

    void setGain(float g) { gain_ = g; }
    void setAttackMs(float ms) { attackMs_ = ms; }
    void setReleaseMs(float ms) { releaseMs_ = ms; }

private:
    struct Cmd { int32_t pitch; int32_t on; };
    struct Voice {
        double phase = 0.0, inc = 0.0;
        float env = 0.0f, target = 0.0f, step = 0.0f;
    };

    void push(int pitch, int on) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) % kCmdCap;
        if (next == tail_.load(std::memory_order_acquire)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        cmds_[h] = Cmd{static_cast<int32_t>(pitch), static_cast<int32_t>(on)};
        head_.store(next, std::memory_order_release);
    }

    bool pop(Cmd& c) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        c = cmds_[t];
        tail_.store((t + 1) % kCmdCap, std::memory_order_release);
        return true;
    }

    void reset() {
        head_.store(0);
        tail_.store(0);
        for (auto& v : voices_) v = Voice{};
    }

    void applyCmd(const Cmd& c) {
        const float aStep = attackMs_ > 0.0f
                                ? 1.0f / (attackMs_ * 0.001f * static_cast<float>(rate_))
                                : 1.0f;
        const float rStep = releaseMs_ > 0.0f
                                ? 1.0f / (releaseMs_ * 0.001f * static_cast<float>(rate_))
                                : 1.0f;
        if (c.pitch < 0) {
            for (auto& v : voices_) { v.target = 0.0f; v.step = rStep; }
            return;
        }
        if (c.pitch >= kPitches) return;
        Voice& v = voices_[static_cast<size_t>(c.pitch)];
        if (c.on) {
            const double f = 440.0 * std::pow(2.0, (c.pitch - 69) / 12.0);
            v.inc = 2.0 * 3.14159265358979323846 * f / static_cast<double>(rate_);
            v.target = 1.0f;
            v.step = aStep;
        } else {
            v.target = 0.0f;
            v.step = rStep;
        }
    }

    static void onCycle(void* ctx, jack_nframes_t frames, int64_t /*cycleStartHns*/) {
        static_cast<JackSineOut*>(ctx)->cycle(frames);
    }

    void cycle(jack_nframes_t frames) {
        float* out = static_cast<float*>(jack_port_get_buffer(port_, frames));
        Cmd c;
        while (pop(c)) applyCmd(c);

        const float g = gain_;
        for (jack_nframes_t i = 0; i < frames; ++i) {
            float s = 0.0f;
            for (auto& v : voices_) {
                if (v.env <= 0.0f && v.target <= 0.0f) continue;
                if (v.env < v.target) v.env = std::min(v.target, v.env + v.step);
                else if (v.env > v.target) v.env = std::max(v.target, v.env - v.step);
                s += v.env * static_cast<float>(std::sin(v.phase));
                v.phase += v.inc;
                if (v.phase > 6.283185307179586) v.phase -= 6.283185307179586;
            }
            out[i] = std::tanh(s * g);
        }
    }

    jack_port_t* port_ = nullptr;
    JackClient::Handler handler_;
    bool started_ = false;
    std::string name_;
    std::vector<std::string> wired_;
    int rate_ = 0;
    UINT32 periodFrames_ = 0;

    std::array<Cmd, kCmdCap> cmds_{};
    std::atomic<size_t> head_{0}, tail_{0};
    std::atomic<uint64_t> dropped_{0};
    std::array<Voice, kPitches> voices_{};
    float gain_ = 0.25f, attackMs_ = 1.5f, releaseMs_ = 40.0f;
};

}  // namespace gdi2m
