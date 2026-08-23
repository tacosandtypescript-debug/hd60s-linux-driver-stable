#!/bin/sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$DIR"

# HD60 S 0fd9:005e → libusb → v4l2loopback (/dev/video42).
# Run as the desktop user so HD60S_AUDIO_PW=1 can reach that user's PipeWire.
# The null sink exposes its monitor as a normal OBS audio input.
SINK_NAME=hd60s_capture
SINK_MODULE=$(pactl list short modules 2>/dev/null | awk -v n="$SINK_NAME" '$2 == "module-null-sink" && $0 ~ ("sink_name=" n "([ ]|$)") {print $1; exit}')
CREATED_SINK=0
if [ -z "$SINK_MODULE" ]; then
  SINK_MODULE=$(pactl load-module module-null-sink sink_name="$SINK_NAME" sink_properties="device.description=HD60 S Capture Audio" 2>/dev/null || true)
  [ -n "$SINK_MODULE" ] && CREATED_SINK=1
fi
if [ -z "$SINK_MODULE" ]; then
  echo "No se pudo crear el sink PipeWire $SINK_NAME" >&2
  exit 1
fi
cleanup() {
  if [ "$CREATED_SINK" -eq 1 ]; then
    pactl unload-module "$SINK_MODULE" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM
env \
  HD60S_INIT_TSV="${HD60S_INIT_TSV:-analysis/init-p2-no9a-no94.tsv}" \
  HD60S_BURST_TSV="${HD60S_BURST_TSV:-analysis/poststream-no9a.tsv}" \
  HD60S_AUDIO_PW=1 \
  HD60S_AUDIO_TARGET="$SINK_NAME" \
  HD60S_PACE_OUTPUT=1 \
  HD60S_VERBOSE=0 \
  ./iso_capture 0 2 1
