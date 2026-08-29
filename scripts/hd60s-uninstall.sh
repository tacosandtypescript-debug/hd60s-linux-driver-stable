#!/usr/bin/env bash
# Quita el driver HD60 S instalado por ./install.sh / make install.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
if [ -f "$SCRIPT_DIR/../Makefile" ]; then
  ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
else
  ROOT=""
fi
PREFIX="${PREFIX:-/usr/local}"

say()  { echo "[hd60s] $*"; }
err()  { echo "[hd60s ERR] $*" >&2; }
die()  { err "$*"; exit 1; }

if [ "$(id -u)" -eq 0 ]; then
  if [ -z "${SUDO_USER:-}" ] || [ "$SUDO_USER" = root ]; then
    die "no lo ejecutes como root. Usa: ./uninstall.sh"
  fi
  REAL_USER="$SUDO_USER"
  as_root() { "$@"; }
else
  REAL_USER="$(id -un)"
  command -v sudo >/dev/null 2>&1 || die "hace falta sudo"
  as_root() { sudo "$@"; }
  as_root -v || die "falló la autenticación de sudo"
fi

REAL_UID="$(id -u "$REAL_USER")"
REAL_HOME="$(getent passwd "$REAL_USER" | cut -d: -f6)"

as_user() {
  if [ "$(id -u)" -eq 0 ]; then
    runuser -u "$REAL_USER" -- env \
      HOME="$REAL_HOME" \
      XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$REAL_UID}" \
      DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$REAL_UID/bus}" \
      "$@"
  else
    "$@"
  fi
}

say "parando el servicio de usuario..."
if [ -S "/run/user/$REAL_UID/bus" ]; then
  as_user systemctl --user disable --now hd60s 2>/dev/null || true
  as_user systemctl --user daemon-reload 2>/dev/null || true
fi

as_root pkill -TERM -x iso_capture 2>/dev/null || true
sleep 1
as_root pkill -9 -x iso_capture 2>/dev/null || true

if [ -n "$ROOT" ] && [ -f "$ROOT/Makefile" ]; then
  say "sudo make uninstall (prefijo $PREFIX)..."
  as_root make -C "$ROOT" PREFIX="$PREFIX" uninstall
else
  say "borrando archivos de $PREFIX..."
  as_root rm -f "$PREFIX/bin/hd60s"
  as_root rm -f /etc/udev/rules.d/70-elgato-hd60s.rules
  as_root rm -f "$PREFIX/share/systemd/user/hd60s.service"
  as_root rm -f /etc/wireplumber/main.lua.d/51-hd60s-alsa.lua
  as_root rm -f /etc/wireplumber/main.lua.d/51-hd60s-v4l2.lua
  as_root rm -f /etc/modprobe.d/hd60s-v4l2loopback.conf /etc/modprobe.d/hd60s-snd-aloop.conf
  as_root rm -f /etc/modules-load.d/hd60s.conf
  as_root rm -f /etc/tmpfiles.d/hd60s.conf
  as_root rm -rf "$PREFIX/libexec/hd60s"
fi

if command -v udevadm >/dev/null 2>&1; then
  as_root udevadm control --reload-rules || true
fi

say "driver desinstalado."
say "los paquetes del sistema (v4l2loopback-dkms, OBS, etc.) se quedan."
say "el módulo v4l2loopback sigue cargado hasta que reinicies o hagas: sudo modprobe -r v4l2loopback"
