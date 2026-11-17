-- snd-aloop (card 10) es un puente interno del driver: iso_capture escribe en
-- hw:10,0 y el modo ALSA lee hw:10,1. Si WirePlumber publica sus nodos, el
-- loopback acaba como salida/entrada por defecto del sistema (el sonido de
-- Linux y el "Desktop Audio"/"Mic" de OBS van a un dispositivo virtual) y,
-- además, PipeWire deja hw:10,0 ocupado. Ninguno de los dos se ofrece.
-- OBS lee el audio del HD60 S desde hd60s_capture (hd60s-audio-bridge.sh).
alsa_monitor.rules = alsa_monitor.rules or {}
table.insert(alsa_monitor.rules, {
  matches = {
    {
      { "node.name", "matches", "alsa_input.platform-snd_aloop.*" },
    },
    {
      { "node.name", "matches", "alsa_output.platform-snd_aloop.*" },
    },
  },
  apply_properties = {
    ["node.disabled"] = true,
  },
})
