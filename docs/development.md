# Development notes

What is in the repository, what was changed in the tracker, how it is measured
and what the measurements were. For playing, see the [user guide](user-guide.md).

## The model

[gdi2m](https://github.com/maximmashkov1/gdi2m) (MIT) is a causal CNN+GRU with
2.2 M parameters. It takes 24 kHz audio and, every 128 samples (5.33 ms),
gives an onset and a sustain probability for each of 53 pitches, MIDI 36 to
88. A decoder turns those into note-on and note-off. Its
[paper](https://github.com/maximmashkov1/gdi2m/blob/main/gdi2m_paper.pdf)
reports an onset F1 of 0.744 on EGDB, an electric-guitar set it never trained
on, and 8.8 ms of algorithmic delay; neither figure was reproduced here.

Upstream publishes its weights only inside the Windows release binary. The
flake's `gdi2m-model` package fetches that binary by hash and runs
`scripts/fetch_model.py` on it, which copies the graph and its two sidecars
out of the resource section; the binary is parsed, never run. The `gdi2m`
package links the model to `share/gdi2m/`, where the tool looks by default.
Without Nix, `python3 scripts/fetch_model.py` downloads and extracts into
`models/gdi2m/`, and `GDI2M_MODEL` names the graph.

## What was changed in `gdi2m/`

`gdi2m/` is [hatch01's Linux port](https://github.com/hatch01/gdi2m) of the
console tool at `a7bd8d8`. All changes are under `gdi2m/tools/gdi2m/`, and
were written with an AI assistant (see the notice in the README):

| Change | Files | Why |
|---|---|---|
| JACK backend for capture, the sine monitor and MIDI out | `jack_client.h`, `jack_capture.h`, `jack_out.h`, `jack_midi.h`, `audio_io.h`, `CMakeLists.txt` | The ALSA backend read whole buffers (42 ms packets on a UR22mkII), could not open a device PipeWire holds, and needed an existing MIDI destination. As JACK ports the tracker runs at the graph's 128-frame period and has a MIDI port of its own. `-DGDI2M_JACK=OFF` still builds the ALSA backend. |
| Inference thread pinned to the fast cores of a hybrid CPU | `platform.h` | Unpinned, inference on this laptop ran at 4.5 ms per 5.33 ms hop and the queue overflowed; pinned it is about 3 ms. |
| Mute gate: release every note when the input level collapses | `gdi2m_core.h` (`MuteGate`, `Decoder::hear`), `--mute-drop-db` | The sustain head holds a damped note for around 300 ms. |
| `gdi2m file --probs` | `main.cpp` | Writes every frame's onset and sustain probabilities, to see why a note was or was not sent. |
| Linux wording in messages and usage | `main.cpp`, `live_engine.h` | |

`gdi2m/export/` and `gdi2m/train/` are upstream's export and training code,
unchanged and so far unused here.

## Build

```sh
nix build .#gdi2m      # result/bin/gdi2m, with the model in result/share/gdi2m
nix develop            # the same binary plus Python with numpy, soundfile, mir_eval
```

The flake only sees files git knows about; `git add -N <file>` is enough for
a new one. The binary links against libjack and finds PipeWire's at run time
through the `LD_LIBRARY_PATH` that NixOS's `services.pipewire.jack` sets.
`scripts/reaper-try.sh` also needs `pw-link` and `reaper` on the PATH.

## Measuring

**Compute.** Per-hop inference time against the 5.33 ms budget:

```sh
gdi2m bench
```

**Accuracy against labels.** `bench/score.py` runs recordings through the
live path and reports onset precision, recall and F1, ghost notes per minute,
recall by chord size and note-release timing:

```sh
python3 bench/score.py guitarset data/guitarset --limit 40
python3 bench/score.py pairs my_takes/        # x.wav + x.tsv (onset_s offset_s midi)
python3 bench/score.py guitarset data/guitarset --match comp -- --threshold 0.8
```

GuitarSet (CC BY 4.0) goes in `data/guitarset/` as `annotation/` and
`audio_mono-pickup_mix/`, unpacked from <https://zenodo.org/records/3371780>.
gdi2m trained on GuitarSet's hexaphonic recordings, so scores on it check the
pipeline and compare settings; they do not say how it does on a new guitar.

**One take in detail.** Record what the model was fed, then see what it heard
at each pick attack and how far the guitar sits from A440:

```sh
gdi2m live --device "Input 2" --no-midi --seconds 30 --dump-resampled data/takes/take.wav
python3 scripts/inspect_take.py data/takes/take.wav
```

**The REAPER path without a guitar.** `scripts/reaper-try.sh` passes its
arguments to the tracker and `scripts/reaper_startup.lua` writes
`.reaper-sandbox/status.txt` once a second (note-ons received, track peak), so
a recording can stand in for playing; `GUITAR_TRACKER_MUTE=1` mutes REAPER's
master:

```sh
GUITAR_TRACKER_MUTE=1 scripts/reaper-try.sh --source file data/takes/take.wav --realtime --play
```

## Results so far

All from 2026-11-15, on a Ryzen AI 9 HX 370 (4 fast and 8 slow cores) with a
Steinberg UR22mkII through PipeWire at 48 kHz, 128-frame period. The machine
was often busy with other work, so compute figures are on the high side.

**Latency and compute**

| | ALSA backend, unpinned | JACK backend, pinned |
|---|---|---|
| Capture packet | 42.5 ms | 2.67 ms |
| Capture to note-on, typical | 47 ms | 5.5 ms |
| Capture to note-on, worst | 629 ms | 6.2 ms |
| Inference per hop, mean / worst | 4.5 / 24.8 ms | 3.0 / 3.9 ms |

Pluck to note-on is the capture-to-note-on figure plus the model's 8.8 ms. A
note leaves on the next JACK cycle, up to 2.7 ms later. ONNX Runtime's profile
puts 51% of inference in convolutions and about a quarter in the slicing and
concatenating that carries streaming state.

**GuitarSet, 40 takes, default settings**

| | |
|---|---|
| Onset precision / recall / F1 at ±50 ms | 0.733 / 0.847 / 0.786 |
| Ghost notes | 104 per minute |
| Recall on notes struck 4 or more together | 0.850 |
| Note release against the labels | median 3 ms early; 5.9% over 100 ms early, 10.5% over 100 ms late |

**Tuning.** 20 GuitarSet chord takes, detuned by resampling, counting a hit on
either neighbouring semitone:

| Detune | Recall | Precision |
|---|---|---|
| in tune | 0.84 | 0.72 |
| 20 cents | 0.82 | 0.73 |
| 40 cents | 0.72 | 0.68 |
| 50 cents | 0.67 | 0.57 |

On the first recordings of the test guitar the strings were 14 to 71 cents
flat and disagreed with each other; 12 of 68 chord notes fired. Tuned to
within 20 cents, 80 of 84 fired over 16 strums of E, A minor, D and G, with 1
wrong sustained note and 8 stray notes under 100 ms. Those chord labels were
read from the take, not annotated by hand.

Lowering the signal by 11 dB or low-passing it heavily cost GuitarSet chord
recall a few points at most, so level and a dull tone are not what breaks
chords.

**Mute gate.** At six abrupt stops in a free-playing take, note-off came a
median 312 ms after the level began to fall with the gate off and 32 ms with
it at 20 dB. On GuitarSet the gate left onsets unchanged, moved the median
release from 14 ms late to 3 ms early and the share over 100 ms late from
13.7% to 10.5%, with the share over 100 ms early going from 5.8% to 5.9%.
Thresholds of 15 and 12 dB gave 27 and 16 ms on that take and were not
checked against GuitarSet.

## Known gaps

- **Compute.** Over half a fast core, with little margin when the machine is
  busy. Too heavy to run inside a DAW's audio callback; a plugin would need
  the inference on a worker thread, or a cheaper engine or model.
- **Stray notes at the attack**, a few milliseconds long.
- **Release of a single note** out of a ringing chord, and of quiet notes
  damped into the noise floor, still waits for the sustain head.
- **No pitch bend.**
- **No held-out test set.** EGDB has not been run here, and the test guitar's
  takes have no hand labels.
- **No tuning warning.** The tracker does not say when the guitar is out of
  tune; `scripts/inspect_take.py` does, after the fact.
- **Personal REAPER configurations** are untested; only the sandboxed REAPER
  that `scripts/reaper-try.sh` opens has been run.
