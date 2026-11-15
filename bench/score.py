#!/usr/bin/env python3
"""Score a transcriber against labelled guitar recordings.

Each recording is pushed through the tracker's LIVE path (device-rate audio,
resampler, causal decoder) and the note events it emits are matched to the
reference with mir_eval. Reported per run, micro-averaged over all notes:

  onset P / R / F1   at each --tolerance (pitch within 50 cents, offsets ignored)
  ghost notes        emitted notes that match nothing, per minute of audio
  onset offset       emitted minus labelled onset over the matched notes
  recall by chord    recall split by how many notes were struck together
                     (reference onsets within 50 ms of each other)
  release            emitted minus labelled note END over the matched notes,
                     and the share released over 100 ms early or late

The onset offset is relative to the LABELS, which in public corpora sit some
milliseconds after the physical attack, so it is not an absolute latency.

    python3 bench/score.py guitarset data/guitarset [--match 05_] [--limit 20]
    python3 bench/score.py pairs my_takes/          # x.wav + x.tsv (onset_s, offset_s, midi)
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import mir_eval
import numpy as np


def guitarset_items(root: Path):
    """(name, wav, reference notes) for every GuitarSet take under root."""
    for jams in sorted((root / "annotation").glob("*.jams")):
        wav = root / "audio_mono-pickup_mix" / f"{jams.stem}_mix.wav"
        if wav.exists():
            yield jams.stem, wav, lambda j=jams: guitarset_notes(j)


def guitarset_notes(jams: Path) -> np.ndarray:
    notes = []
    for ann in json.loads(jams.read_text())["annotations"]:
        if ann["namespace"] != "note_midi":
            continue
        for obs in ann["data"]:
            notes.append((obs["time"], obs["time"] + obs["duration"], obs["value"]))
    return np.array(sorted(notes), dtype=float).reshape(-1, 3)


def pair_items(root: Path):
    """(name, wav, reference notes) for every x.wav with an x.tsv beside it."""
    for wav in sorted(root.glob("*.wav")):
        tsv = wav.with_suffix(".tsv")
        if tsv.exists():
            yield wav.stem, wav, lambda t=tsv: np.loadtxt(t, ndmin=2, comments="#")[:, :3]


def transcribe(gdi2m: str, model: Path, wav: Path, extra: list[str]) -> np.ndarray:
    """Run the tracker over wav; rows of (onset_s, offset_s, midi pitch)."""
    side = json.loads(model.with_suffix(".json").read_text())["config"]
    frame_s = side["hop_length"] / side["sample_rate"]
    with tempfile.TemporaryDirectory() as tmp:
        events = Path(tmp) / "events.tsv"
        cmd = [gdi2m, "live", "--source", "file", str(wav), "--events", str(events),
               "--no-midi", "--quiet", "--model", str(model), *extra]
        run = subprocess.run(cmd, capture_output=True, text=True)
        if run.returncode != 0 or not events.exists():
            raise RuntimeError(f"{' '.join(cmd)}\n{run.stdout[-2000:]}{run.stderr[-2000:]}")
        rows = np.loadtxt(events, skiprows=1, ndmin=2)
    if rows.size == 0:
        return np.zeros((0, 3))
    return np.column_stack([rows[:, 0] * frame_s, rows[:, 1] * frame_s, rows[:, 2]])


def match(ref: np.ndarray, est: np.ndarray, tolerance_s: float):
    """Index pairs (ref, est) of notes matched on onset and pitch."""
    if len(ref) == 0 or len(est) == 0:
        return []
    hz = lambda midi: 440.0 * 2.0 ** ((midi - 69.0) / 12.0)
    # mir_eval wants positive durations; offsets are not scored
    ref_iv = np.column_stack([ref[:, 0], np.maximum(ref[:, 1], ref[:, 0] + 1e-3)])
    est_iv = np.column_stack([est[:, 0], np.maximum(est[:, 1], est[:, 0] + 1e-3)])
    return mir_eval.transcription.match_notes(
        ref_iv, hz(ref[:, 2]), est_iv, hz(est[:, 2]),
        onset_tolerance=tolerance_s, pitch_tolerance=50.0, offset_ratio=None)


def struck_together(ref: np.ndarray, window_s: float = 0.05) -> np.ndarray:
    """For each reference note, how many reference onsets (itself included) fall within window_s."""
    onsets = ref[:, 0]
    return np.array([np.sum(np.abs(onsets - t) <= window_s) for t in onsets], dtype=int)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("layout", choices=["guitarset", "pairs"])
    ap.add_argument("root", type=Path)
    ap.add_argument("--match", default="", help="only recordings whose name contains this")
    ap.add_argument("--limit", type=int, default=0, help="at most this many recordings")
    ap.add_argument("--tolerance", type=float, nargs="+", default=[50.0, 20.0], metavar="MS")
    ap.add_argument("--gdi2m", default="gdi2m", help="tracker binary")
    ap.add_argument("--model", type=Path,
                    default=Path(os.environ.get("GDI2M_MODEL", "models/gdi2m/gdi2m.onnx")))
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--per-file", action="store_true", help="also print one line per recording")
    ap.add_argument("tracker_args", nargs="*", metavar="-- ARGS",
                    help="passed through to the tracker, e.g. -- --threshold 0.9")
    args = ap.parse_args()

    items = list({"guitarset": guitarset_items, "pairs": pair_items}[args.layout](args.root))
    items = [it for it in items if args.match in it[0]]
    if args.limit:
        items = items[:: max(1, len(items) // args.limit)][: args.limit]
    if not items:
        print(f"no recordings under {args.root}", file=sys.stderr)
        return 1

    def one(item):
        name, wav, load_ref = item
        return name, load_ref(), transcribe(args.gdi2m, args.model, wav, args.tracker_args)

    with ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(one, items))

    minutes = sum(max(ref[:, 1].max(initial=0), est[:, 1].max(initial=0))
                  for _, ref, est in results) / 60.0
    n_ref = sum(len(ref) for _, ref, _ in results)
    n_est = sum(len(est) for _, _, est in results)
    print(f"{len(results)} recordings, {minutes:.1f} min, {n_ref} reference notes, {n_est} emitted")

    for tol_ms in args.tolerance:
        hits, deltas, releases = 0, [], []
        chord_total, chord_hit = np.zeros(5, dtype=int), np.zeros(5, dtype=int)
        for name, ref, est in results:
            pairs = match(ref, est, tol_ms / 1000.0)
            hits += len(pairs)
            size = np.minimum(struck_together(ref), 4)
            np.add.at(chord_total, size, 1)
            np.add.at(chord_hit, size[[r for r, _ in pairs]], 1)
            deltas += [est[e, 0] - ref[r, 0] for r, e in pairs]
            releases += [est[e, 1] - ref[r, 1] for r, e in pairs]
            if args.per_file:
                p, r = len(pairs) / max(len(est), 1), len(pairs) / max(len(ref), 1)
                print(f"  {name:<32} P {p:.3f}  R {r:.3f}  F1 {2 * p * r / max(p + r, 1e-9):.3f}")
        p, r = hits / max(n_est, 1), hits / max(n_ref, 1)
        d = np.array(deltas) * 1000.0 if deltas else np.zeros(1)
        print(f"±{tol_ms:g} ms  P {p:.3f}  R {r:.3f}  F1 {2 * p * r / max(p + r, 1e-9):.3f}"
              f"   ghosts {(n_est - hits) / minutes:.1f}/min   missed {(n_ref - hits) / minutes:.1f}/min"
              f"   onset offset median {np.median(d):+.1f} ms, p10 {np.percentile(d, 10):+.1f},"
              f" p90 {np.percentile(d, 90):+.1f}")
        rel = np.array(releases) * 1000.0 if releases else np.zeros(1)
        print(f"        release: median {np.median(rel):+.0f} ms, p10 {np.percentile(rel, 10):+.0f}, p90 {np.percentile(rel, 90):+.0f}"
              f"   over 100 ms early {100 * np.mean(rel < -100):.1f}%   over 100 ms late {100 * np.mean(rel > 100):.1f}%")
        print("        recall by notes struck together:  " + "   ".join(
            f"{'4+' if k == 4 else k}: {chord_hit[k] / chord_total[k]:.3f} (n={chord_total[k]})"
            for k in range(1, 5) if chord_total[k]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
