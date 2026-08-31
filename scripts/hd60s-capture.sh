#!/bin/sh
# Supervisor: espera el USB 0fd9:005e y relanza iso_capture.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -x "$SCRIPT_DIR/iso_capture" ]; then
  DIR="$SCRIPT_DIR"
elif [ -x "$SCRIPT_DIR/../iso_capture" ]; then
  DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
else
  DIR="$SCRIPT_DIR"
fi
cd "$DIR"

# HD60 S 0fd9:005e → libusb → v4l2loopback (/dev/video42).
usb_device_present() {
  for usb in /sys/bus/usb/devices/*; do
    [ -r "$usb/idVendor" ] || continue
    [ "$(cat "$usb/idVendor" 2>/dev/null)" = "0fd9" ] || continue
    [ "$(cat "$usb/idProduct" 2>/dev/null)" = "005e" ] && return 0
  done
  return 1
}

device_state=unknown
child_pid=0
hold_pid=0

start_hold() {
  if [ "$hold_pid" -gt 0 ] 2>/dev/null && kill -0 "$hold_pid" 2>/dev/null; then
    return 0
  fi
  if pgrep -x iso_capture >/dev/null 2>&1; then
    return 0
  fi
  if [ -x "$SCRIPT_DIR/hd60s-hold-loopback.sh" ]; then
    sh "$SCRIPT_DIR/hd60s-hold-loopback.sh" &
    hold_pid=$!
    echo "[hd60s] sujetando /dev/video42 (negro 1080p60) para OBS"
  elif [ -x "$DIR/hd60s-hold-loopback.sh" ]; then
    sh "$DIR/hd60s-hold-loopback.sh" &
    hold_pid=$!
    echo "[hd60s] sujetando /dev/video42 (negro 1080p60) para OBS"
  fi
}

stop_hold() {
  if [ "$hold_pid" -gt 0 ] 2>/dev/null; then
    kill -TERM "$hold_pid" 2>/dev/null || true
    wait "$hold_pid" 2>/dev/null || true
    hold_pid=0
  fi
  pkill -f 'ffmpeg.*video42' 2>/dev/null || true
}

link_hd60s_play() {
  command -v pw-link >/dev/null 2>&1 || return 0
  sink=${HD60S_PW_SINK:-hd60s_out}
  # Reintenta un poco: el stream nativo tarda en aparecer.
  n=0
  while [ "$n" -lt 20 ]; do
    if pw-link "hd60s_play:output_FL" "$sink:playback_FL" 2>/dev/null; then
      pw-link "hd60s_play:output_FR" "$sink:playback_FR" 2>/dev/null || true
      echo "[hd60s] audio enlazado hd60s_play → $sink"
      return 0
    fi
    n=$((n + 1))
    sleep 0.25
  done
  return 0
}

ensure_hd60s_audio_source() {
  # Puente permanente PipeWire: sink hd60s_out + fuente hd60s_capture.
  # OBS no pierde el dispositivo cuando iso_capture se reinicia.
  if [ -x "$SCRIPT_DIR/hd60s-audio-bridge.sh" ]; then
    sh "$SCRIPT_DIR/hd60s-audio-bridge.sh" || true
  fi
  case "${HD60S_AUDIO_PW:-0}" in
    0|n|N) ;;
    *) return 0 ;;
  esac
  command -v pactl >/dev/null 2>&1 || {
    echo "[hd60s] pactl no está disponible; no se pudo publicar el audio" >&2
    return 0
  }

  audio_source_name=${HD60S_AUDIO_SOURCE_NAME:-hd60s_capture}
  audio_capture_dev=${HD60S_AUDIO_CAPTURE_DEV:-hw:10,1}

  # PipeWire-Pulse may not be ready when a user service starts.
  tries=0
  while ! pactl info >/dev/null 2>&1 && [ "$tries" -lt 20 ]; do
    tries=$((tries + 1))
    sleep 1
  done
  if ! pactl info >/dev/null 2>&1; then
    echo "[hd60s] PipeWire-Pulse no está disponible; se omite la fuente de audio" >&2
    return 0
  fi

  if pactl list short sources | awk -v name="$audio_source_name" '$2 == name { found = 1 } END { exit !found }'; then
    return 0
  fi

  module_id=$(pactl load-module module-alsa-source \
    "device=$audio_capture_dev" \
    "source_name=$audio_source_name" \
    format=s16le rate=48000 channels=1 \
    source_properties=device.description=Elgato-HD60S-Audio 2>/dev/null || true)
  if [ -n "$module_id" ]; then
    echo "[hd60s] fuente de audio $audio_source_name publicada desde $audio_capture_dev"
  else
    echo "[hd60s] no se pudo publicar la fuente de audio $audio_source_name" >&2
  fi
}

stop_supervisor() {
  trap - INT TERM HUP
  if [ "$child_pid" -gt 0 ] 2>/dev/null; then
    kill -TERM "$child_pid" 2>/dev/null || true
    wait "$child_pid" 2>/dev/null || true
  fi
  stop_hold
  exit 0
}

trap stop_supervisor INT TERM HUP

while :; do
  if ! usb_device_present; then
    if [ "$device_state" != absent ]; then
      echo "[hd60s] HD60 S no enumerada; esperando reconexión USB"
      device_state=absent
    fi
    start_hold
    sleep 2
    continue
  fi
  if [ "$device_state" != present ]; then
    echo "[hd60s] HD60 S enumerada; iniciando captura"
    device_state=present
  fi
  echo "[hd60s] iniciando captura; se reintentará automáticamente tras una desconexión"
  ensure_hd60s_audio_source
  stop_hold
  sleep 0.2
  set +e
  # snd-aloop is a persistent OBS-facing source across iso_capture restarts.
  env \
    HD60S_INIT_TSV="${HD60S_INIT_TSV:-analysis/init-p2-audio-fast.tsv}" \
    HD60S_BURST_TSV="${HD60S_BURST_TSV:-analysis/poststream-no9a.tsv}" \
    HD60S_AUDIO_PW="${HD60S_AUDIO_PW:-1}" \
    HD60S_POST_ISO_AUDIO94="${HD60S_POST_ISO_AUDIO94:-1}" \
    HD60S_AUDIO="${HD60S_AUDIO:-1}" \
    HD60S_AUDIO_V2="${HD60S_AUDIO_V2:-1}" \
    HD60S_IT6802_RECOVER="${HD60S_IT6802_RECOVER:-1}" \
    HD60S_RECOVER_MS="${HD60S_RECOVER_MS:-100}" \
    HD60S_CADENCE_DIAG="${HD60S_CADENCE_DIAG:-0}" \
    HD60S_PACE_OUTPUT=1 \
    HD60S_VERBOSE=0 \
    ./iso_capture 0 2 1 &
  child_pid=$!
  link_hd60s_play &
  # Anunciar 60 fps en el lado CAPTURE para que OBS no negocie 30.
  (
    i=0
    while [ "$i" -lt 20 ]; do
      if v4l2-ctl -d /dev/video42 --set-parm=60 >/dev/null 2>&1; then
        break
      fi
      i=$((i + 1))
      sleep 0.5
    done
  ) &
  wait "$child_pid"
  rc=$?
  child_pid=0
  set -e
  echo "[hd60s] iso_capture terminó (rc=$rc); esperando reconexión USB"
  sleep 2
done
