#!/usr/bin/env bash
# Try the tracker in REAPER.
#
# Opens a throwaway REAPER - its own config under .reaper-sandbox/, never
# ~/.config/REAPER - with one ReaSynth track listening on its MIDI input, and
# runs gdi2m wired into that input. Closing REAPER ends the session.
#
#   scripts/reaper-try.sh [gdi2m live args...]      default: --device "Input 2"
#
#   GUITAR_TRACKER_SINK   part of the name of the output to play through
#                         (default UR22mkII); REAPER otherwise lands on the
#                         default sink, which may be Bluetooth
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
if ! command -v gdi2m >/dev/null; then
    exec nix develop "$repo" -c "$0" "$@"
fi
export GDI2M_MODEL=${GDI2M_MODEL:-$repo/models/gdi2m/gdi2m.onnx}

box=$repo/.reaper-sandbox
cfg=$box/xdg/REAPER
mkdir -p "$cfg/Scripts"
# JACK mode, so REAPER is one more client of the PipeWire graph gdi2m is in:
# its ALSA mode would take the interface for itself. One MIDI input, enabled;
# no audio inputs, so nothing reaches for the default (Bluetooth) microphone.
[ -f "$cfg/reaper.ini" ] || printf '%s\n' '[REAPER]' linux_audio_mode=0 \
    linux_audio_nch_in=0 linux_audio_nch_out=2 jack_midiins=1 jack_midiouts=0 \
    midiins_all=1 splashscreen=0 > "$cfg/reaper.ini"
cp "$repo/scripts/reaper_startup.lua" "$cfg/Scripts/__startup.lua"

export GUITAR_TRACKER_STATUS=$box/status.txt
rm -f "$GUITAR_TRACKER_STATUS"

reaper_pid='' tracker_pid=''
cleanup() {
    [ -n "$tracker_pid" ] && kill -INT "$tracker_pid" 2>/dev/null && wait "$tracker_pid" 2>/dev/null
    [ -n "$reaper_pid" ] && kill "$reaper_pid" 2>/dev/null
    true
}
trap cleanup EXIT

XDG_CONFIG_HOME=$box/xdg PIPEWIRE_LATENCY=128/48000 \
    reaper -cfgfile "$cfg/reaper.ini" -newinst -nosplash >"$box/reaper.log" 2>&1 &
reaper_pid=$!

midi_in='REAPER:MIDI Input 1'
for _ in $(seq 150); do
    pw-link -i | grep -qxF "$midi_in" && break
    kill -0 "$reaper_pid" 2>/dev/null || { echo "REAPER exited; see $box/reaper.log" >&2; exit 1; }
    sleep 0.2
done
pw-link -i | grep -qxF "$midi_in" || { echo "REAPER never opened '$midi_in'" >&2; exit 1; }

# Move REAPER's outputs from wherever JACK auto-connected them to the chosen sink.
sink=${GUITAR_TRACKER_SINK:-UR22mkII}
mapfile -t playback < <(pw-link -i | grep -F "$sink" | grep -F ':playback_')
if [ "${#playback[@]}" -ge 2 ]; then
    for n in 1 2; do
        out="REAPER:out$n"
        pw-link -l -o | awk -v port="$out" '$0 == port {on = 1; next} /^[^ ]/ {on = 0} on && /\|->/ {sub(/^ *\|-> /, ""); print}' |
            while IFS= read -r peer; do pw-link -d "$out" "$peer" || true; done
        pw-link "$out" "${playback[$((n - 1))]}" || true
    done
else
    echo "no playback ports matching '$sink'; REAPER stays on the default sink" >&2
fi

[ "$#" -gt 0 ] || set -- --device "Input 2"
gdi2m live "$@" --port "$midi_in" &
tracker_pid=$!

wait "$reaper_pid" || true
reaper_pid=''
