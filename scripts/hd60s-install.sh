#!/usr/bin/env bash
# Instala el driver userspace de la Elgato HD60 S (0fd9:005e).
# Uso: ./install.sh [--extras] [--deps-only] [--no-service]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
if [ -f "$SCRIPT_DIR/../Makefile" ]; then
  ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
else
  ROOT="$SCRIPT_DIR"
fi

PREFIX="${PREFIX:-/usr/local}"
EXTRAS=0
DEPS_ONLY=0
NO_SERVICE=0

say()  { echo "[hd60s] $*"; }
err()  { echo "[hd60s ERR] $*" >&2; }
die()  { err "$*"; exit 1; }

usage() {
  cat <<EOF
Instalador del driver Linux de la Elgato HD60 S.

Uso: ./install.sh [opciones]

  --extras      instala también OBS, mpv, VLC, tmux y ffmpeg
  --deps-only   solo paquetes del sistema (no compila ni instala el driver)
  --no-service  no activa el servicio systemd de usuario
  -h, --help    esta ayuda

Tras instalar, enchufa el HD60 S (USB 3.0) y en OBS elige:
  vídeo  → Dispositivo de captura de video (V4L2) → ElgatoHD60S
  audio  → hd60s_capture
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --extras) EXTRAS=1 ;;
    --deps-only) DEPS_ONLY=1 ;;
    --no-service) NO_SERVICE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) die "opción desconocida: $1 (prueba --help)" ;;
  esac
  shift
done

[ "$(uname -s)" = Linux ] || die "este driver solo funciona en Linux"

if [ "$(id -u)" -eq 0 ]; then
  if [ -z "${SUDO_USER:-}" ] || [ "$SUDO_USER" = root ]; then
    die "no lo ejecutes como root. Usa: ./install.sh  (pedirá sudo)"
  fi
  REAL_USER="$SUDO_USER"
  as_root() { "$@"; }
else
  REAL_USER="$(id -un)"
  command -v sudo >/dev/null 2>&1 || die "hace falta sudo"
  as_root() { sudo "$@"; }
fi

REAL_UID="$(id -u "$REAL_USER")"
REAL_HOME="$(getent passwd "$REAL_USER" | cut -d: -f6)"
[ -n "$REAL_HOME" ] || die "no pude resolver el home de $REAL_USER"

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

if [ "$(id -u)" -ne 0 ]; then
  say "hace falta sudo para paquetes, udev y /usr/local"
  as_root -v || die "falló la autenticación de sudo"
fi

detect_pm() {
  if command -v apt-get >/dev/null 2>&1; then
    echo apt
  elif command -v dnf >/dev/null 2>&1; then
    echo dnf
  elif command -v pacman >/dev/null 2>&1; then
    echo pacman
  else
    echo none
  fi
}

PM="$(detect_pm)"
[ "$PM" != none ] || die "no reconozco el gestor de paquetes (hace falta apt, dnf o pacman)"

APT_OPTS=(-y --no-install-recommends --no-upgrade)

install_apt() {
  local pkgs=(
    build-essential pkg-config git curl ca-certificates
    libusb-1.0-0-dev libasound2-dev libpipewire-0.3-dev libsamplerate0-dev
    v4l2loopback-utils
    pipewire pipewire-pulse wireplumber
    alsa-utils usbutils
  )
  if apt-cache show "linux-headers-$(uname -r)" >/dev/null 2>&1; then
    pkgs+=("linux-headers-$(uname -r)")
  else
    say "aviso: no hay linux-headers-$(uname -r); hace falta para v4l2loopback"
  fi
  say "apt: actualizando índices..."
  as_root env DEBIAN_FRONTEND=noninteractive apt-get update -y
  say "apt: instalando dependencias de compilación y runtime..."
  as_root env DEBIAN_FRONTEND=noninteractive apt-get install "${APT_OPTS[@]}" "${pkgs[@]}"
  if [ "$EXTRAS" -eq 1 ]; then
    say "apt: extras (OBS, mpv, tmux, ffmpeg)..."
    as_root env DEBIAN_FRONTEND=noninteractive apt-get install "${APT_OPTS[@]}" \
      ffmpeg vlc mpv guvcview tmux obs-studio || \
      say "aviso: no se pudieron instalar todos los extras"
  fi
}

install_dnf() {
  local pkgs=(
    gcc make pkgconf git curl
    libusb1-devel alsa-lib-devel pipewire-devel libsamplerate-devel
    kernel-devel
    pipewire pipewire-pulseaudio wireplumber
    alsa-utils usbutils
  )
  say "dnf: instalando dependencias..."
  as_root dnf install -y "${pkgs[@]}"
  as_root dnf install -y v4l2loopback v4l2loopback-utils 2>/dev/null || true
  if [ "$EXTRAS" -eq 1 ]; then
    as_root dnf install -y ffmpeg vlc mpv guvcview tmux obs-studio || \
      say "aviso: no se pudieron instalar todos los extras"
  fi
}

install_pacman() {
  local pkgs=(
    base-devel git curl
    libusb alsa-lib pipewire libsamplerate
    linux-headers
    pipewire-pulse wireplumber
    alsa-utils usbutils
  )
  say "pacman: instalando dependencias..."
  as_root pacman -Sy --needed --noconfirm "${pkgs[@]}"
  as_root pacman -S --needed --noconfirm v4l2loopback-dkms v4l2loopback-utils 2>/dev/null || true
  if [ "$EXTRAS" -eq 1 ]; then
    as_root pacman -S --needed --noconfirm ffmpeg vlc mpv guvcview tmux obs-studio || \
      say "aviso: no se pudieron instalar todos los extras"
  fi
}

have_v4l2loopback_kmod() {
  modinfo v4l2loopback >/dev/null 2>&1
}

install_v4l2loopback_upstream() {
  local ver="0.15.4"
  local url="https://github.com/v4l2loopback/v4l2loopback/archive/refs/tags/v${ver}.tar.gz"
  local tmp src
  say "compilando v4l2loopback ${ver} para este kernel (el paquete de la distro no sirve)..."
  tmp="$(mktemp -d)"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" | tar -xz -C "$tmp"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO- "$url" | tar -xz -C "$tmp"
  else
    rm -rf "$tmp"
    die "hace falta curl o wget para bajar v4l2loopback"
  fi
  src="$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | head -n1)"
  [ -n "$src" ] && [ -f "$src/Makefile" ] || die "no pude desempaquetar v4l2loopback"
  as_root make -C "$src"
  as_root make -C "$src" install
  as_root depmod -a
  rm -rf "$tmp"
  have_v4l2loopback_kmod || die "v4l2loopback se compiló pero el kernel no lo ve. ¿linux-headers?"
}

ensure_v4l2loopback_kmod() {
  if have_v4l2loopback_kmod; then
    say "módulo v4l2loopback ya está disponible en este kernel"
    return 0
  fi
  case "$PM" in
    apt)
      say "apt: intentando v4l2loopback-dkms..."
      if ! as_root env DEBIAN_FRONTEND=noninteractive apt-get install "${APT_OPTS[@]}" v4l2loopback-dkms; then
        say "dkms de Ubuntu no compila en este kernel; lo quito y uso upstream"
        as_root dpkg --remove --force-remove-reinstreq v4l2loopback-dkms 2>/dev/null || true
      fi
      ;;
  esac
  if have_v4l2loopback_kmod; then
    return 0
  fi
  install_v4l2loopback_upstream
}

install_packages() {
  case "$PM" in
    apt) install_apt ;;
    dnf) install_dnf ;;
    pacman) install_pacman ;;
  esac
  ensure_v4l2loopback_kmod
}

ensure_video_group() {
  if id -nG "$REAL_USER" | tr ' ' '\n' | grep -qx video; then
    return 0
  fi
  say "añadiendo $REAL_USER al grupo video (hace falta para /dev/video42)"
  as_root usermod -aG video "$REAL_USER"
  say "aviso: cierra sesión y vuelve a entrar para que el grupo video aplique"
}

loopback_loaded_ok() {
  [ -e /dev/video42 ] || return 1
  lsmod | grep -q v4l2loopback || return 1
  return 0
}

ensure_modules() {
  as_root systemd-tmpfiles --create /etc/tmpfiles.d/hd60s.conf 2>/dev/null || true

  if loopback_loaded_ok; then
    say "/dev/video42 ya existe, no recargo v4l2loopback"
  else
    say "cargando v4l2loopback (ElgatoHD60S, exclusive_caps=1)..."
    as_root modprobe v4l2loopback video_nr=42 card_label=ElgatoHD60S exclusive_caps=1 \
      || die "no pude cargar v4l2loopback. ¿Están los headers del kernel y v4l2loopback-dkms?"
  fi
  if ! lsmod | grep -q '^snd_aloop'; then
    as_root modprobe snd-aloop enable=1 index=10 id=hd60s pcm_substreams=1 || \
      say "aviso: no pude cargar snd-aloop (el audio PipeWire nativo no lo necesita)"
  fi
  if command -v udevadm >/dev/null 2>&1; then
    as_root udevadm control --reload-rules || true
    as_root udevadm trigger --subsystem-match=usb || true
    as_root udevadm trigger --subsystem-match=video4linux || true
  fi
}

maybe_restart_wireplumber() {
  local dst="/etc/wireplumber/main.lua.d/51-hd60s-v4l2.lua"
  local src="$ROOT/packaging/wireplumber/51-hd60s-v4l2.lua"
  [ -f "$src" ] || return 0
  if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
    return 0
  fi
  NEED_WP_RESTART=1
}

enable_service() {
  [ "$NO_SERVICE" -eq 0 ] || { say "omitido el servicio (--no-service)"; return 0; }
  if [ ! -S "/run/user/$REAL_UID/bus" ]; then
    say "aviso: no hay bus de sesión de $REAL_USER; activa luego: systemctl --user enable --now hd60s"
    return 0
  fi
  as_root loginctl enable-linger "$REAL_USER" 2>/dev/null || \
    say "aviso: no pude activar linger (el servicio solo arranca con la sesión)"
  as_user systemctl --user daemon-reload
  if as_user systemctl --user is-enabled --quiet hd60s 2>/dev/null; then
    :
  else
    as_user systemctl --user enable hd60s
  fi
  if as_user systemctl --user is-active --quiet hd60s 2>/dev/null; then
    say "servicio hd60s ya estaba en marcha (no lo reinicio)"
  else
    as_user systemctl --user start hd60s
    say "servicio de usuario hd60s arrancado"
  fi
  if [ "${NEED_WP_RESTART:-0}" -eq 1 ]; then
    say "reiniciando WirePlumber para que no se quede con /dev/video42"
    as_user systemctl --user try-restart wireplumber.service 2>/dev/null || true
  fi
}

print_done() {
  cat <<EOF

────────────────────────────────────────
Listo. El driver queda instalado en ${PREFIX}.

  comando:     hd60s
  vídeo:       /dev/video42  (ElgatoHD60S, 1080p60 YUYV)
  audio:       hd60s_capture (PipeWire, 48 kHz estéreo)
  servicio:    systemctl --user status hd60s

En OBS:
  1. Fuente → Dispositivo de captura de video (V4L2) → ElgatoHD60S
  2. Fuente de audio → hd60s_capture

Diagnóstico:  hd60s doctor
Parar:        hd60s stop
Quitar:       ./uninstall.sh
────────────────────────────────────────
EOF
}

NEED_WP_RESTART=0

say "instalando para el usuario $REAL_USER (prefijo $PREFIX, gestor $PM)"
install_packages
ensure_video_group

if [ "$DEPS_ONLY" -eq 1 ]; then
  say "solo paquetes (--deps-only). Compila con: make all && sudo make install"
  exit 0
fi

[ -f "$ROOT/Makefile" ] || die "no encuentro el Makefile. Clona el repo y ejecuta ./install.sh desde la raíz"
command -v make >/dev/null 2>&1 || die "no está make (¿falló la instalación de paquetes?)"
command -v pkg-config >/dev/null 2>&1 || die "no está pkg-config"
pkg-config --exists libusb-1.0 || die "falta libusb-1.0 (dev)"
pkg-config --exists libpipewire-0.3 || die "falta libpipewire-0.3 (dev)"

maybe_restart_wireplumber
say "compilando iso_capture..."
as_user make -C "$ROOT" -j"$(nproc 2>/dev/null || echo 1)" all
say "instalando a $PREFIX (udev, v4l2loopback, systemd, WirePlumber)..."
as_root make -C "$ROOT" PREFIX="$PREFIX" install
ensure_modules
enable_service
print_done
if [ -x "$ROOT/scripts/hd60s" ]; then
  as_user "$ROOT/scripts/hd60s" doctor || true
elif command -v hd60s >/dev/null 2>&1; then
  as_user hd60s doctor || true
fi
