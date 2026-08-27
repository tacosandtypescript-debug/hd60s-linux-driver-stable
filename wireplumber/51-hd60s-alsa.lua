-- snd-aloop cross-connects iso_capture's hw:10,0 playback to hw:10,1
-- capture. WirePlumber's ACP profile exposes hw:10,0 capture instead,
-- which is the opposite direction and must not be offered to OBS.
alsa_monitor.rules = alsa_monitor.rules or {}
table.insert(alsa_monitor.rules, {
  matches = {
    {
      { "node.name", "matches", "alsa_input.platform-snd_aloop.0.analog-stereo" },
    },
  },
  apply_properties = {
    ["node.disabled"] = true,
  },
})
