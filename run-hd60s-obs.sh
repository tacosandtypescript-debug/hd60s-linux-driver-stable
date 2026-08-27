#!/bin/sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
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
while :; do
  if ! usb_device_present; then
    if [ "$device_state" != absent ]; then
      echo "[hd60s] HD60 S no enumerada; esperando reconexión USB"
      device_state=absent
    fi
    sleep 2
    continue
  fi
  if [ "$device_state" != present ]; then
    echo "[hd60s] HD60 S enumerada; iniciando captura"
    device_state=present
  fi
  echo "[hd60s] iniciando captura; se reintentará automáticamente tras una desconexión"
  set +e
  env \
    HD60S_INIT_TSV="${HD60S_INIT_TSV:-analysis/init-p2-no9a-no94.tsv}" \
    HD60S_BURST_TSV="${HD60S_BURST_TSV:-analysis/poststream-no9a.tsv}" \
    HD60S_AUDIO_PW=1 \
    HD60S_PACE_OUTPUT=1 \
    HD60S_VERBOSE=0 \
    ./iso_capture 0 2 1
  rc=$?
  set -e
  echo "[hd60s] iso_capture terminó (rc=$rc); esperando reconexión USB"
  sleep 2
done
