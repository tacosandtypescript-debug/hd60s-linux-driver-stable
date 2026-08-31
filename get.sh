#!/usr/bin/env bash
# Instalación de un comando: driver + OBS 1080p60 y abre OBS.
#
#   bash <(curl -fsSL https://raw.githubusercontent.com/tacosandtypescript-debug/hd60s-linux-driver-stable/main/get.sh)
#
# bash <(...) deja la terminal para sudo. No uses "curl | bash" si pide contraseña.
set -euo pipefail

REPO_URL="${HD60S_REPO:-https://github.com/tacosandtypescript-debug/hd60s-linux-driver-stable.git}"
BRANCH="${HD60S_BRANCH:-main}"
DEST="${HD60S_SRC:-$HOME/hd60s-linux-driver-stable}"
RAW_GET="https://raw.githubusercontent.com/tacosandtypescript-debug/hd60s-linux-driver-stable/${BRANCH}/get.sh"

say() { echo "[hd60s] $*"; }
die() { echo "[hd60s ERR] $*" >&2; exit 1; }

[ "$(uname -s)" = Linux ] || die "este driver solo funciona en Linux"
[ "$(id -u)" -ne 0 ] || die "no lo ejecutes como root. Usa: bash <(curl -fsSL $RAW_GET)"

if [ ! -t 0 ] && [ ! -t 1 ]; then
  die "hace falta una terminal (sudo pide contraseña). Ejecuta:
  bash <(curl -fsSL $RAW_GET)"
fi

need() {
  command -v "$1" >/dev/null 2>&1
}

if ! need git || ! need curl; then
  say "instalando git/curl..."
  if need apt-get; then
    sudo apt-get update -y
    sudo apt-get install -y --no-install-recommends git curl ca-certificates
  elif need dnf; then
    sudo dnf install -y git curl
  elif need pacman; then
    sudo pacman -Sy --needed --noconfirm git curl
  else
    die "instala git y curl y vuelve a lanzar el instalador"
  fi
fi

if [ -d "$DEST/.git" ]; then
  say "actualizando $DEST ..."
  git -C "$DEST" fetch origin "$BRANCH"
  git -C "$DEST" checkout "$BRANCH"
  git -C "$DEST" pull --ff-only origin "$BRANCH"
else
  say "clonando en $DEST ..."
  git clone --branch "$BRANCH" --depth 1 "$REPO_URL" "$DEST"
fi

exec bash "$DEST/install.sh" --extras --open-obs
