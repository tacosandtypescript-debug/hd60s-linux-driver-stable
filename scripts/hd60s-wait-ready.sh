#!/bin/sh
# Espera a que iso_capture esté escribiendo 1920x1080 en /dev/video42.
# Tras un reboot OBS se abre antes y el loopback aún no es CAPTURE (exclusive_caps).
set -eu
VDEV="${VDEV:-/dev/video42}"
timeout="${HD60S_WAIT_SEC:-120}"
i=0
echo "[hd60s] esperando captura 1920x1080 en $VDEV (máx ${timeout}s)..."
while [ "$i" -lt "$timeout" ]; do
  if [ -e "$VDEV" ] && command -v v4l2-ctl >/dev/null 2>&1; then
    fmt=$(v4l2-ctl -d "$VDEV" --get-fmt-video 2>/dev/null || true)
    case "$fmt" in
      *1920/1080*)
        v4l2-ctl -d "$VDEV" --set-parm=60 >/dev/null 2>&1 || true
        echo "[hd60s] listo: $VDEV 1920x1080 @60"
        exit 0
        ;;
    esac
  fi
  i=$((i + 1))
  sleep 1
done
echo "[hd60s] aviso: timeout esperando $VDEV 1920x1080" >&2
exit 1
