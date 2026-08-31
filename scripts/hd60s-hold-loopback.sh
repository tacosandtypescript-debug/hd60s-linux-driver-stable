#!/bin/sh
# Mantiene /dev/video42 como productor 1080p60 para que OBS no lo abra
# como OUTPUT (exclusive_caps) al cerrar/abrir sin iso_capture.
set -eu
VDEV="${VDEV:-/dev/video42}"

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "[hd60s] ffmpeg no está; no puedo sujetar $VDEV" >&2
  exit 1
fi

# Ya hay escritor (iso_capture u otro hold).
if command -v fuser >/dev/null 2>&1; then
  if fuser "$VDEV" >/dev/null 2>&1; then
    if pgrep -x iso_capture >/dev/null 2>&1; then
      exit 0
    fi
  fi
fi

exec ffmpeg -nostdin -hide_banner -loglevel error \
  -re -f lavfi -i color=c=black:s=1920x1080:r=60 \
  -pix_fmt yuyv422 -f v4l2 "$VDEV"
