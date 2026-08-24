#!/bin/sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$DIR"

# HD60 S 0fd9:005e → libusb → v4l2loopback (/dev/video42).
env \
  HD60S_INIT_TSV="${HD60S_INIT_TSV:-analysis/init-p2-no9a-no94.tsv}" \
  HD60S_BURST_TSV="${HD60S_BURST_TSV:-analysis/poststream-no9a.tsv}" \
  HD60S_AUDIO_PW=1 \
  HD60S_PACE_OUTPUT=1 \
  HD60S_VERBOSE=0 \
  ./iso_capture 0 2 1
