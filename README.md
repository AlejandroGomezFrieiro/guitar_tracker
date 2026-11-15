# guitar_tracker

Polyphonic guitar to MIDI in real time on Linux, from an ordinary pickup and
an audio interface. Play chords or single notes into the interface; notes come
out on a MIDI port that a synth or a DAW can use.

The tracker is [gdi2m](https://github.com/maximmashkov1/gdi2m), a small causal
neural network, with a JACK/PipeWire backend and a few fixes added here.

## Quick start

```sh
nix develop          # builds the tracker, fetches the pretrained model
gdi2m selftest
gdi2m live --list    # find your guitar input
```

To install it instead, add this flake's `gdi2m` package to your profile or
your NixOS or Home Manager configuration; the model comes with it.

Tune the guitar to A440, then hear what it detects as plain sine tones:

```sh
gdi2m live --device "Input 2" --sine --sine-device ur22 --no-midi
```

Or open it in REAPER with a synth already wired up:

```sh
scripts/reaper-try.sh
```

The [user guide](docs/user-guide.md) covers setup, playing, REAPER and what to
do when it misbehaves.

## What to expect

Measured on a Ryzen AI 9 HX 370 laptop with a Steinberg UR22mkII, 128-frame
period at 48 kHz:

| | |
|---|---|
| Pluck to note-on | about 14 ms: 8.8 ms for the model to see the attack, 5.5 ms from capture to the note leaving |
| Damping the strings to note-off | about 32 ms |
| Open chords on a tuned electric guitar | 80 of 84 chord notes over 16 strums, 1 wrong sustained note, 8 stray notes under 100 ms |
| CPU | about 3 ms of every 5.33 ms on one fast core |

It needs a guitar tuned to A440: a quarter tone out, chords fall apart. It
does not track pitch bends, and releasing one note out of a ringing chord is
slower than damping everything. Details and the public-dataset scores are in
[docs/development.md](docs/development.md).

## Layout

| Path | What |
|---|---|
| `gdi2m/` | the tracker's source: a copy of gdi2m's Linux port, changed here |
| `scripts/fetch_model.py` | extracts the pretrained model (the flake runs it; by hand only without Nix) |
| `scripts/reaper-try.sh` | opens a throwaway REAPER wired to the tracker |
| `scripts/inspect_take.py` | shows what the model heard in a recording, and how far out of tune it was |
| `bench/score.py` | scores the tracker against labelled recordings |
| `docs/` | user guide, development notes |
| `data/` | recordings and datasets; not in git |

## AI assistance

Everything in this repository outside upstream's gdi2m code was written with
an AI coding assistant: Claude (Anthropic), running in Claude Code under my
direction, on 2026-11-15. That covers the changes in `gdi2m/tools/gdi2m/`
(the JACK backend, core pinning, the mute gate, `--probs`), the scripts, the
bench, and the documentation, this file included.

The measurements in [docs/development.md](docs/development.md) were run on my
machine during that work, and I tested the result by playing through it. The
AI-written code has not had a line-by-line human review, so read it with that
in mind.

## Credits and licences

- This repository is MIT licensed, see `LICENSE`.
- gdi2m by Maxim Mashkov Izvekov, also MIT, in `gdi2m/LICENSE`. Linux port of
  the console tool by [hatch01](https://github.com/hatch01/gdi2m).
- The pretrained model is the one upstream ships in its 0.1.0 release.
- GuitarSet, used by the bench, is CC BY 4.0.
