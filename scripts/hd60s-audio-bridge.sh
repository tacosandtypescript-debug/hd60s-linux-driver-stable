#!/bin/sh
# Fuente PipeWire permanente (hd60s_capture) que no muere con iso_capture.
# iso_capture reproduce hacia el sink hd60s_out; OBS lee el monitor remapeado.
set -eu

audio_source_name=${HD60S_AUDIO_SOURCE_NAME:-hd60s_capture}
audio_sink_name=${HD60S_PW_SINK:-hd60s_out}

say() { echo "[hd60s-audio] $*"; }

command -v pactl >/dev/null 2>&1 || {
  say "pactl no está; se omite el puente de audio"
  exit 0
}

tries=0
while ! pactl info >/dev/null 2>&1 && [ "$tries" -lt 25 ]; do
  tries=$((tries + 1))
  sleep 1
done
if ! pactl info >/dev/null 2>&1; then
  say "PipeWire-Pulse no está listo"
  exit 0
fi

sink_exists() {
  pactl list short sinks 2>/dev/null | awk -v n="$audio_sink_name" '$2 == n { found = 1 } END { exit !found }'
}

source_exists() {
  pactl list short sources 2>/dev/null | awk -v n="$audio_source_name" '$2 == n { found = 1 } END { exit !found }'
}

if ! sink_exists; then
  if pactl load-module module-null-sink \
      "sink_name=$audio_sink_name" \
      "rate=48000" "channels=2" "format=s16le" \
      "sink_properties=device.description=Elgato-HD60S" \
      >/dev/null; then
    say "sink $audio_sink_name creado"
  else
    say "no pude crear $audio_sink_name"
    exit 0
  fi
fi

# Espera el monitor (a veces tarda un instante).
mon="${audio_sink_name}.monitor"
tries=0
while ! pactl list short sources 2>/dev/null | awk -v n="$mon" '$2 == n { found = 1 } END { exit !found }' \
      && [ "$tries" -lt 15 ]; do
  tries=$((tries + 1))
  sleep 0.2
done

if source_exists; then
  # Ya hay hd60s_capture (remap previo o el stream nativo). No duplicar.
  exit 0
fi

if pactl load-module module-remap-source \
    "source_name=$audio_source_name" \
    "master=$mon" \
    "channels=2" \
    "channel_map=front-left,front-right" \
    "source_properties=device.description=Elgato-HD60S-Audio" \
    >/dev/null; then
  say "fuente permanente $audio_source_name → $mon"
else
  say "no pude remapear $audio_source_name (OBS puede usar $mon)"
fi
exit 0
