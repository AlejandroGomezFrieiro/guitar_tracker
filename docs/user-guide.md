# User guide

How to set up, play through the tracker and get the notes into a synth or a
DAW. Commands assume you are inside `nix develop` at the repository root, with
the model fetched (see the README).

## Before you play

**Tune to A440.** This matters more than anything else. The model only knows
equal-tempered semitones, and a string a quarter tone out lands between two of
them, so chords come out as one note or none. Any whole number of semitones
down (E flat, D standard) is fine. Use a tuner set to A = 440 Hz.

**Give it a clean signal.** Guitar straight into the interface's
instrument (Hi-Z) input, no effects. Bridge pickup and tone fully up track
best.

**Set the level.** Play normally for a few seconds and stop the tracker; the
summary says how far the input was from the level the model was trained at and
what `--gain` would correct it:

```sh
gdi2m live --device "Input 2" --no-midi --seconds 10
```

Raising the interface's input knob does the same as `--gain`. Leave some
headroom: if the summary reports blocks at full scale, back the knob off.

## Find your ports

```sh
gdi2m live --list
```

This prints three lists: capture ports (your guitar input is one of them),
render devices (for the sine monitor) and MIDI outputs. Anywhere a port is
asked for you can give its number from the list or a unique part of its name,
such as `"Input 2"` or `ur22`.

## Hear what it detects

The quickest check needs no synth. Each detected note is played as a sine:

```sh
gdi2m live --device "Input 2" --sine --sine-device ur22 --no-midi
```

- `--sine-device` picks where the sines go. Without it they go to the default
  output, and if that is Bluetooth you will hear a long delay that has nothing
  to do with the tracker.
- `--sine-gain 0.1` makes the monitor quieter (default 0.25).
- Ctrl-C stops it and prints the session summary.

## Send notes to a synth or a DAW

Without `--no-midi` the notes leave on a MIDI port of the tracker's own,
`gdi2m:midi_out`:

```sh
gdi2m live --device "Input 2"
```

Wire that port to an instrument in a patchbay (qpwgraph, Helvum), or let the
tracker wire it for you by naming the destination:

```sh
gdi2m live --device "Input 2" --port "REAPER:MIDI Input 1"
```

With no `--device`, nothing is wired to the tracker's input `gdi2m:in`; send
audio to it from a patchbay or a DAW track instead.

## REAPER

```sh
scripts/reaper-try.sh
```

This opens a REAPER with one track, "Guitar to MIDI", armed and playing
ReaSynth from the tracker's notes. Closing REAPER stops the tracker.

- It is a throwaway REAPER with its own settings in `.reaper-sandbox/`. Your
  own REAPER configuration is not read or changed, so your theme, plugins and
  licence are not there.
- Sound goes to the UR22mkII's outputs. Name another output with
  `GUITAR_TRACKER_SINK=<part of its name>`.
- Anything after the script name is passed to the tracker, replacing the
  default `--device "Input 2"`:

```sh
scripts/reaper-try.sh --device "Input 1" --gain 2
```

To use your own REAPER, three things have to be true. This route has not been
tried against a personal configuration; the script above is the tested one.

1. REAPER's audio system is JACK (Preferences > Audio > Device). In ALSA mode
   REAPER takes the interface for itself and the tracker cannot open it.
2. A MIDI input is enabled under Preferences > MIDI Devices, and the tracker
   is wired to it (`--port "REAPER:MIDI Input 1"`).
3. A track with an instrument is armed, monitoring, with that MIDI input.

## Options worth knowing

| Option | What it does |
|---|---|
| `--gain G` | scales the input before the model; the summary tells you the value |
| `--threshold F` | how sure the model must be to start a note (default 0.85). Lower adds a few chord notes and more wrong ones; higher loses real notes quickly |
| `--mute-drop-db N` | how far the level must fall for damped strings to end every note (default 20). Lower is quicker and riskier; 0 turns it off |
| `--attack-velocity` | note velocity follows how hard you pick, instead of a fixed 100 |
| `--channel N` | MIDI channel, 1 to 16 |
| `--split P:C` | notes from pitch P upward go to channel C, for a bass/lead split |
| `--seconds N` | stop by itself after N seconds |
| `--quiet` | no level meter while running |

`gdi2m` with no arguments prints all of them.

## Reading the session summary

When a session ends the tracker prints what happened. The lines that matter:

- **hops dropped, peak queue depth**: both should be 0 and 1 or 2. Dropped
  hops mean the computer could not keep up, and notes will be late or missing.
- **input level**: how far you were from the trained level, and the `--gain`
  to use.
- **capture -> note-on**: time from the audio arriving to the note leaving,
  about 5 to 6 ms when healthy. Add about 9 ms, which is how long the model
  needs to see a pick attack, for pluck-to-note.
- **inference/frame**: must stay under 5.33 ms. Around 3 ms is normal here.

## When something is wrong

**Chords come out as one note, or notes are a semitone off.** The guitar is
out of tune. Record ten seconds and look:

```sh
mkdir -p data/takes
gdi2m live --device "Input 2" --no-midi --seconds 10 --dump-resampled data/takes/check.wav
python3 scripts/inspect_take.py data/takes/check.wav
```

The second line of the output says how many cents the strings sit from A440
semitones. More than about 20, retune.

**There is a long delay.** Check where the sound is going first: Bluetooth
headphones add far more than the tracker does. Then look at the summary: a
full queue and dropped hops mean the CPU is busy with something else.

**Notes hang after I stop the strings.** Damping everything ends the notes in
about 30 ms. A quiet note, or one note lifted out of a chord that keeps
ringing, is released by the model alone and takes a few hundred milliseconds.
`--mute-drop-db 15` reacts sooner.

**Short wrong notes at the attack.** A hard pick attack can fire stray notes a
few milliseconds long. A synth patch with a slower attack hides them. Raising
`--threshold` is a poor trade: at 0.9 a test take went from 8 stray notes to
3 and lost a quarter of its real ones.

**Nothing is detected.** With no `--device` the input is not wired to
anything. Check the level meter moves while you play, and that the port you
named is the one your guitar is plugged into.

**"could not open a MIDI port".** The destination named by `--port` does not
exist or matches more than one port. `gdi2m live --list` shows the names.

## What it does not do

- Pitch bends and vibrato are not sent: every note is a whole semitone.
- Two strings playing the same pitch are one note.
- It listens to one guitar signal. It does not know which string a note was
  played on.
