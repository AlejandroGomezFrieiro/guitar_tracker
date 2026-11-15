#!/usr/bin/env python3
"""Show what the model heard at each pick attack of a take.

For a take recorded with `gdi2m live ... --dump-resampled take.wav` (the audio
exactly as the model was fed it), find the attacks in the waveform and print,
for each one, every pitch whose onset head moved:

  fired       onset probability crossed the threshold: a note was sent
  under       it rose, but not far enough: the model heard something there
  sustain     onset stayed flat, yet the sustain head says the pitch is ringing

A chord that comes out as one note shows up here as one `fired` pitch with the
rest of the chord under `under` or `sustain` - or with nothing at all, which
is the model not hearing those strings.

    python3 scripts/inspect_take.py data/takes/chords.wav [--threshold 0.85]
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note(midi: int) -> str:
    return f"{NAMES[midi % 12]}{midi // 12 - 1}"


def attacks(x: np.ndarray, hop: int, rise_db: float = 9.0, floor_db: float = -50.0,
            gap_frames: int = 28) -> list[int]:
    """Frames where the level jumps by rise_db within three hops, at least gap_frames apart."""
    frames = len(x) // hop
    rms = np.sqrt(np.mean(x[: frames * hop].reshape(frames, hop) ** 2, axis=1))
    db = 20 * np.log10(np.maximum(rms, 1e-7))
    found, last = [], -gap_frames
    for f in range(3, frames):
        if db[f] > floor_db and db[f] - db[f - 3 : f].min() > rise_db and f - last >= gap_frames:
            found.append(f)
            last = f
    return found


def tuning(x: np.ndarray, rate: int) -> tuple[float, float]:
    """How far the take's partials sit from A440 semitones.

    Returns (cents, share): the typical offset of the spectral peaks from the
    nearest equal-tempered pitch, and the share of peak energy more than 30
    cents from any. The pitch rows are A440 semitones, so a guitar a quarter
    tone out lands between two rows and splits its probability across both.
    """
    n, hop = 8192, 2048
    window = np.hanning(n)
    lo, hi = int(70 * n / rate), int(700 * n / rate)
    cents, weight = [], []
    for i in range(0, len(x) - n, hop):
        mag = np.abs(np.fft.rfft(x[i : i + n] * window))
        strong = 0.15 * mag[lo:hi].max()
        for k in range(lo, hi):
            if mag[k] > strong and mag[k] > mag[k - 1] and mag[k] >= mag[k + 1]:
                a, b, c = np.log(mag[k - 1 : k + 2] + 1e-12)
                midi = 69 + 12 * np.log2((k + 0.5 * (a - c) / (a - 2 * b + c)) * rate / n / 440.0)
                cents.append(100 * (midi - round(midi)))
                weight.append(mag[k])
    if not cents:
        return 0.0, 0.0
    cents, weight = np.array(cents), np.array(weight)
    # offsets wrap at +-50 cents, so average them on a circle
    mean = np.angle(np.sum(weight * np.exp(2j * np.pi * cents / 100))) * 100 / (2 * np.pi)
    return float(mean), float(weight[np.abs(cents) > 30].sum() / weight.sum())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("wav", type=Path, help="take at the model's sample rate")
    ap.add_argument("--threshold", type=float, help="onset threshold (default: the model's)")
    ap.add_argument("--floor", type=float, default=0.2, help="smallest probability worth listing")
    ap.add_argument("--gdi2m", default="gdi2m", help="tracker binary")
    ap.add_argument("--model", type=Path,
                    default=Path(os.environ.get("GDI2M_MODEL", "models/gdi2m/gdi2m.onnx")))
    args = ap.parse_args()

    cfg = json.loads(args.model.with_suffix(".json").read_text())["config"]
    hop, rate, low = cfg["hop_length"], cfg["sample_rate"], cfg["min_pitch"]
    threshold = args.threshold or cfg["onset_threshold"]

    x, wav_rate = sf.read(args.wav, dtype="float32", always_2d=True)
    if wav_rate != rate:
        print(f"{args.wav} is {wav_rate} Hz; record it with --dump-resampled ({rate} Hz)", file=sys.stderr)
        return 1
    x = x.mean(axis=1)

    with tempfile.TemporaryDirectory() as tmp:
        probs_path = Path(tmp) / "probs.tsv"
        run = subprocess.run([args.gdi2m, "file", str(args.wav), "--probs", str(probs_path),
                              "--model", str(args.model)], capture_output=True, text=True)
        if run.returncode != 0 or not probs_path.exists():
            print(run.stdout[-2000:] + run.stderr[-2000:], file=sys.stderr)
            return 1
        probs = np.loadtxt(probs_path, skiprows=1, ndmin=2)
    pitches = probs.shape[1] // 2
    onset, sustain = probs[:, :pitches], probs[:, pitches:]

    found = attacks(x, hop)
    frame_ms = 1000.0 * hop / rate
    print(f"{args.wav}: {len(x) / rate:.1f} s, {len(found)} attacks, onset threshold {threshold:g}")
    offset, off_pitch = tuning(x, rate)
    print(f"tuning: partials sit {offset:+.0f} cents from A440 semitones, "
          f"{100 * off_pitch:.0f}% of their energy more than 30 cents out\n")
    n_fired = n_under = n_sustain = 0
    for f in found:
        # the onset head peaks within a few frames of the attack; the sustain
        # head is read once the transient has passed
        on = onset[max(f - 2, 0) : f + 13].max(axis=0)
        su = sustain[f + 20 : f + 40].mean(axis=0) if f + 20 < len(sustain) else np.zeros(pitches)
        fired = [(p, on[p]) for p in range(pitches) if on[p] >= threshold]
        under = [(p, on[p]) for p in range(pitches) if args.floor <= on[p] < threshold]
        ringing = [(p, su[p]) for p in range(pitches) if su[p] >= 0.5 and on[p] < args.floor]
        n_fired, n_under, n_sustain = n_fired + len(fired), n_under + len(under), n_sustain + len(ringing)
        show = lambda rows: "  ".join(f"{note(low + p)} {v:.2f}" for p, v in rows) or "-"
        print(f"{f * frame_ms / 1000:7.2f}s  fired    {show(fired)}")
        print(f"          under    {show(under)}")
        print(f"          sustain  {show(ringing)}")
    if found:
        n = len(found)
        print(f"\nper attack: {n_fired / n:.1f} fired, {n_under / n:.1f} under threshold, "
              f"{n_sustain / n:.1f} heard by the sustain head only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
