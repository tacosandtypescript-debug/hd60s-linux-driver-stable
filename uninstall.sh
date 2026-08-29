#!/usr/bin/env bash
# Quita el driver instalado por ./install.sh.
set -euo pipefail
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
exec bash "$DIR/scripts/hd60s-uninstall.sh" "$@"
