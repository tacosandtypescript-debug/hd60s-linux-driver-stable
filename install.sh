#!/usr/bin/env bash
# Instalador de un comando: dependencias, compilar, udev, loopback y servicio.
set -euo pipefail
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
exec bash "$DIR/scripts/hd60s-install.sh" "$@"
