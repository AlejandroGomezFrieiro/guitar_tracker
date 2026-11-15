-- Startup script for the throwaway REAPER that scripts/reaper-try.sh opens.
--
-- Makes one track that plays whatever arrives on REAPER's MIDI input through
-- ReaSynth, and writes once a second what has arrived and how loud the track
-- got, so the session can be checked without looking at the window.
--
--   GUITAR_TRACKER_STATUS   file to write the status line to
--   GUITAR_TRACKER_MUTE=1   mute the master (a check that must not be heard)

local status_path = os.getenv("GUITAR_TRACKER_STATUS")
local ALL_MIDI_INPUTS = 4096 + (63 << 5)   -- every device, every channel

local track
local function setup()
  if reaper.CountTracks(0) == 0 then
    reaper.InsertTrackAtIndex(0, false)
    track = reaper.GetTrack(0, 0)
    reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "Guitar to MIDI", true)
    reaper.TrackFX_AddByName(track, "ReaSynth", false, -1)
    reaper.SetMediaTrackInfo_Value(track, "I_RECINPUT", ALL_MIDI_INPUTS)
    reaper.SetMediaTrackInfo_Value(track, "I_RECMON", 1)
    reaper.SetMediaTrackInfo_Value(track, "I_RECARM", 1)
    -- six ReaSynth voices at full level peak above 0 dB
    reaper.SetMediaTrackInfo_Value(track, "D_VOL", 10 ^ (-12 / 20))
    reaper.TrackList_AdjustWindows(false)
  end
  track = reaper.GetTrack(0, 0)
  if os.getenv("GUITAR_TRACKER_MUTE") == "1" then
    reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), "B_MUTE", 1)
  end
end

-- Note-ons since launch. The input history is newest first, each event with
-- a sequence number that only grows, so a pass stops at the last one counted.
local seen, note_ons = 0, 0
local function count_note_ons()
  local newest = reaper.MIDI_GetRecentInputEvent(0)
  for i = 0, 255 do
    local seq, msg = reaper.MIDI_GetRecentInputEvent(i)
    if seq == 0 or seq <= seen then break end
    if #msg >= 3 and msg:byte(1) & 0xF0 == 0x90 and msg:byte(3) > 0 then note_ons = note_ons + 1 end
  end
  seen = math.max(seen, newest)
end

local frames, peak, last_write = 0, 0.0, 0.0
local function tick()
  frames = frames + 1
  -- a few frames in, so the project REAPER opens at launch is the one set up
  if frames == 10 then setup() end
  if track and reaper.ValidatePtr2(0, track, "MediaTrack*") then
    peak = math.max(peak, reaper.Track_GetPeakInfo(track, 0))
    count_note_ons()
    local now = reaper.time_precise()
    if status_path and now - last_write >= 1.0 then
      last_write = now
      local f = io.open(status_path, "w")
      if f then
        f:write(string.format("note_ons=%d track_peak_db=%.1f audio=%s\n", note_ons,
                              peak > 0 and 20 * math.log(peak, 10) or -150.0,
                              select(2, reaper.GetAudioDeviceInfo("MODE", ""))))
        f:close()
      end
    end
  end
  reaper.defer(tick)
end
reaper.defer(tick)
