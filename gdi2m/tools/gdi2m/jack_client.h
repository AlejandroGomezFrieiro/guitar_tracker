// jack_client.h - the one JACK client every Linux endpoint hangs off.
//
// ALSA gives each endpoint its own handle and its own thread; JACK gives a
// process ONE client and ONE process callback. So capture (jack_capture.h),
// the sine monitor (jack_out.h) and MIDI out (jack_midi.h) are three ports on
// a shared client, and this owns the callback they all run in.
//
// Under PipeWire this is its JACK shim: the ports show up in the graph next to
// every other node, a DAW or patchbay can wire them, and the period is the
// graph's quantum rather than a private ALSA buffer - which is the point. The
// ALSA backend reads whole buffers (42 ms packets on a UR22mkII) and cannot
// share a device PipeWire already holds.
//
// The callback is the audio thread: no allocation, no lock, no I/O. Endpoints
// attach as a function pointer + context published through an atomic, and
// detach by clearing it and waiting out the cycle that may still be using it.
#pragma once

#include <jack/jack.h>
#include <jack/midiport.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "platform.h"

namespace gdi2m {

class JackClient {
public:
    // What an endpoint runs each cycle. `cycleStartHns` is when this cycle's
    // period finished being captured, on nowHns()'s clock.
    struct Handler {
        void (*fn)(void* ctx, jack_nframes_t frames, int64_t cycleStartHns) = nullptr;
        void* ctx = nullptr;
    };

    static JackClient& get() {
        static JackClient instance;
        return instance;
    }

    // Idempotent. The client is activated straight away: ports can be
    // registered and connected afterwards, and a connection made before
    // activation is refused.
    bool open(std::string& err) {
        if (client_) return true;
        // PipeWire sizes the graph's quantum from what its nodes ASK for, and a
        // node that asks for nothing inherits whatever the browser wanted. One
        // 128-frame period is half a model hop; an explicit setting wins.
        setenv("PIPEWIRE_LATENCY", "128/48000", 0);
        jack_status_t status{};
        client_ = jack_client_open("gdi2m", JackNoStartServer, &status);
        if (!client_) {
            err = "cannot reach a JACK server (is PipeWire's JACK support, or jackd, running?)";
            return false;
        }
        jack_set_process_callback(client_, &JackClient::process, this);
        jack_set_xrun_callback(client_, &JackClient::xrun, this);
        if (jack_activate(client_) != 0) {
            err = "jack_activate failed";
            jack_client_close(client_);
            client_ = nullptr;
            return false;
        }
        return true;
    }

    jack_client_t* handle() const { return client_; }
    int rate() const { return client_ ? static_cast<int>(jack_get_sample_rate(client_)) : 0; }
    UINT32 period() const { return client_ ? jack_get_buffer_size(client_) : 0; }
    uint64_t xruns() const { return xruns_.load(std::memory_order_relaxed); }

    // Every port of `type` carrying all of `flags`, minus this client's own.
    std::vector<std::string> ports(const char* type, unsigned long flags) const {
        std::vector<std::string> out;
        if (!client_) return out;
        const std::string self = std::string(jack_get_client_name(client_)) + ":";
        const char** names = jack_get_ports(client_, nullptr, type, flags);
        for (const char** n = names; n && *n; ++n)
            if (std::string(*n).rfind(self, 0) != 0) out.emplace_back(*n);
        if (names) jack_free(names);
        return out;
    }

    // Registered once and kept for the life of the client: a port that comes
    // and goes drops whatever a patchbay or session had wired to it.
    jack_port_t* port(const char* name, const char* type, unsigned long flags) {
        if (!client_) return nullptr;
        if (jack_port_t* p = jack_port_by_name(client_, fullName(name).c_str())) return p;
        return jack_port_register(client_, name, type, flags, 0);
    }
    std::string fullName(const char* port) const {
        return std::string(jack_get_client_name(client_)) + ":" + port;
    }

    void attachCapture(const Handler* h) { capture_.store(h, std::memory_order_release); }
    void attachRender(const Handler* h) { render_.store(h, std::memory_order_release); }
    void detachCapture() { capture_.store(nullptr, std::memory_order_release); settle(); }
    void detachRender() { render_.store(nullptr, std::memory_order_release); settle(); }

    // ---- MIDI out ---------------------------------------------------------
    // JACK MIDI can only be written from inside the callback, into that
    // cycle's buffer, so a send is a lock-free hand-off and the note leaves at
    // the top of the next cycle: at most one period after the decoder fired.
    void openMidi() {
        midiPort_.store(port("midi_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput),
                        std::memory_order_release);
    }
    bool midiOpen() const { return midiPort_.load(std::memory_order_acquire) != nullptr; }

    // SPSC; a full ring DROPS rather than blocks, same policy as the sine ring.
    bool sendMidi(unsigned char st, unsigned char d1, unsigned char d2) {
        const size_t h = midiHead_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) % kMidiCap;
        if (next == midiTail_.load(std::memory_order_acquire)) return false;
        midi_[h] = {st, d1, d2};
        midiHead_.store(next, std::memory_order_release);
        return true;
    }

    // Returns once a full cycle has started and finished after the call, so
    // nothing detached before it is still running and everything queued before
    // it is on the wire.
    void settle() const {
        if (!client_) return;
        const uint64_t seen = cycles_.load(std::memory_order_acquire);
        for (int i = 0; i < 200 && cycles_.load(std::memory_order_acquire) < seen + 2; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

private:
    static constexpr size_t kMidiCap = 1024;

    JackClient() = default;
    ~JackClient() {
        if (client_) jack_client_close(client_);
    }

    static int xrun(void* arg) {
        static_cast<JackClient*>(arg)->xruns_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    static int process(jack_nframes_t frames, void* arg) {
        auto* self = static_cast<JackClient*>(arg);

        jack_nframes_t cycleFrames = 0;
        jack_time_t cycleUsecs = 0, nextUsecs = 0;
        float periodUsecs = 0.0f;
        const int64_t startHns =
            jack_get_cycle_times(self->client_, &cycleFrames, &cycleUsecs, &nextUsecs,
                                 &periodUsecs) == 0
                ? static_cast<int64_t>(cycleUsecs) * 10
                : nowHns();

        if (const Handler* h = self->capture_.load(std::memory_order_acquire))
            h->fn(h->ctx, frames, startHns);

        if (jack_port_t* p = self->midiPort_.load(std::memory_order_acquire)) {
            void* buf = jack_port_get_buffer(p, frames);
            jack_midi_clear_buffer(buf);
            size_t t = self->midiTail_.load(std::memory_order_relaxed);
            while (t != self->midiHead_.load(std::memory_order_acquire)) {
                // A refused write means this cycle's buffer is full; the rest
                // goes out with the next one.
                if (jack_midi_event_write(buf, 0, self->midi_[t].data(), 3) != 0) break;
                t = (t + 1) % kMidiCap;
            }
            self->midiTail_.store(t, std::memory_order_release);
        }

        if (const Handler* h = self->render_.load(std::memory_order_acquire))
            h->fn(h->ctx, frames, startHns);

        self->cycles_.fetch_add(1, std::memory_order_release);
        return 0;
    }

    jack_client_t* client_ = nullptr;
    std::atomic<const Handler*> capture_{nullptr}, render_{nullptr};
    std::atomic<jack_port_t*> midiPort_{nullptr};
    std::array<std::array<unsigned char, 3>, kMidiCap> midi_{};
    std::atomic<size_t> midiHead_{0}, midiTail_{0};
    std::atomic<uint64_t> cycles_{0}, xruns_{0};
};

}  // namespace gdi2m
