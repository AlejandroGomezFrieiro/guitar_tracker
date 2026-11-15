// JACK MIDI out - the JACK twin of alsa_midi.h and gdi2m_core.h's winmm MidiOut.
//
// Same surface and the same index-into-names convention, with one difference
// that is the reason to prefer it: the tool does not need a destination to
// exist. Index 0 is its own `gdi2m:midi_out` port left unwired - what loopMIDI
// is on Windows, without the second program - for a DAW to pick as a MIDI
// input or a patchbay to route. Every later index is an existing MIDI input
// port that `open` wires `midi_out` to.
#pragma once

#include <cerrno>
#include <string>
#include <vector>

#include "jack_client.h"

namespace gdi2m {

inline std::vector<std::string> midiPortNames() {
    std::vector<std::string> names;
    std::string err;
    JackClient& jc = JackClient::get();
    if (!jc.open(err)) return names;
    names.push_back(jc.fullName("midi_out") + "  (own port, nothing wired)");
    for (auto& p : jc.ports(JACK_DEFAULT_MIDI_TYPE, JackPortIsInput))
        names.push_back(std::move(p));
    return names;
}

class MidiOut {
public:
    MidiOut() = default;
    bool open(int index) {
        close();
        std::string err;
        JackClient& jc = JackClient::get();
        if (!jc.open(err)) return false;
        const auto dests = jc.ports(JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
        if (index < 0 || index > static_cast<int>(dests.size())) return false;
        jc.openMidi();
        if (!jc.midiOpen()) return false;
        if (index > 0) {
            // EEXIST is a wire a session manager already made, not a failure.
            const int rc = jack_connect(jc.handle(), jc.fullName("midi_out").c_str(),
                                        dests[static_cast<size_t>(index - 1)].c_str());
            if (rc != 0 && rc != EEXIST) return false;
        }
        open_ = true;
        return true;
    }

    // Split out of the destructor: the engine has to be able to release a
    // port without being destroyed, because a failed start unwinds past an
    // already-open one and a restart re-opens it.
    void close() {
        if (!open_) return;
        allNotesOff();
        JackClient::get().settle();   // or the note-offs die in the ring
        open_ = false;
    }
    ~MidiOut() { close(); }

    void send(unsigned char st, unsigned char d1, unsigned char d2) const {
        if (open_) JackClient::get().sendMidi(st, d1, d2);
    }
    void noteOn(int ch0, int n, int v) const {
        send(static_cast<unsigned char>(0x90 | (ch0 & 15)),
             static_cast<unsigned char>(n), static_cast<unsigned char>(v));
    }
    void noteOff(int ch0, int n) const {
        send(static_cast<unsigned char>(0x80 | (ch0 & 15)),
             static_cast<unsigned char>(n), 0);
    }
    void controlChange(int ch0, int cc, int v) const {
        send(static_cast<unsigned char>(0xB0 | (ch0 & 15)),
             static_cast<unsigned char>(cc), static_cast<unsigned char>(v));
    }
    void allNotesOff() const {
        for (int ch = 0; ch < 16; ++ch)
            send(static_cast<unsigned char>(0xB0 | ch), 123, 0);
    }
    bool valid() const { return open_; }

private:
    bool open_ = false;
};

}  // namespace gdi2m
