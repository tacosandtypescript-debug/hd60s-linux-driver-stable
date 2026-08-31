#!/usr/bin/env bash
# Perfil y colección OBS HD60 S (1920x1080 @ 60) y, opcionalmente, abre OBS.
set -euo pipefail

LAUNCH=1
WAIT=1
while [ $# -gt 0 ]; do
  case "$1" in
    --no-launch) LAUNCH=0 ;;
    --no-wait) WAIT=0 ;;
    -h|--help)
      echo "Uso: hd60s-obs-setup.sh [--no-launch] [--no-wait]"
      exit 0
      ;;
    *) echo "[hd60s ERR] opción desconocida: $1" >&2; exit 1 ;;
  esac
  shift
done

say() { echo "[hd60s] $*"; }

# Un solo arranque a la vez (systemd + autostart del escritorio).
exec 9>"/tmp/hd60s-obs-setup.lock"
if ! flock -n 9; then
  say "otro hd60s-obs-setup ya está en marcha"
  exit 0
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "$WAIT" -eq 1 ] && [ -x "$SCRIPT_DIR/hd60s-wait-ready.sh" ]; then
  "$SCRIPT_DIR/hd60s-wait-ready.sh" || say "aviso: abro OBS aunque el loopback aún no marque 1080p60"
fi

OBS_HOME="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio"
SCENE_DIR="$OBS_HOME/basic/scenes"
PROFILE_DIR="$OBS_HOME/basic/profiles/HD60S"
GLOBAL="$OBS_HOME/global.ini"
VDEV="${VDEV:-/dev/video42}"

mkdir -p "$SCENE_DIR" "$PROFILE_DIR"

if command -v v4l2-ctl >/dev/null 2>&1 && [ -e "$VDEV" ]; then
  v4l2-ctl -d "$VDEV" --set-parm=60 >/dev/null 2>&1 || true
fi

python3 - "$SCENE_DIR/HD60S.json" <<'PY'
import json, sys, uuid

path = sys.argv[1]
pack = lambda a, b: (a << 16) | (b & 0xffff)
v_uuid = str(uuid.uuid4())
a_uuid = str(uuid.uuid4())
scene_uuid = str(uuid.uuid4())
desk_uuid = str(uuid.uuid4())
mic_uuid = str(uuid.uuid4())

v4l2 = {
    "prev_ver": 503316482,
    "name": "Elgato HD60 S",
    "uuid": v_uuid,
    "id": "v4l2_input",
    "versioned_id": "v4l2_input",
    "settings": {
        "device_id": "/dev/video42",
        "input": 0,
        "pixelformat": 1448695129,
        "resolution": pack(1920, 1080),
        "framerate": pack(1, 60),
        "buffering": False,
        "auto_reset": True,
    },
    "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
    "enabled": True, "muted": False, "push-to-mute": False, "push-to-mute-delay": 0,
    "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {},
    "deinterlace_mode": 0, "deinterlace_field_order": 0, "monitoring_type": 0,
    "private_settings": {},
}
audio = {
    "prev_ver": 503316482,
    "name": "Elgato HD60 S Audio",
    "uuid": a_uuid,
    "id": "pulse_input_capture",
    "versioned_id": "pulse_input_capture",
    "settings": {"device_id": "hd60s_capture"},
    "mixers": 255, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
    "enabled": True, "muted": False, "push-to-mute": False, "push-to-mute-delay": 0,
    "push-to-talk": False, "push-to-talk-delay": 0,
    "hotkeys": {"libobs.mute": [], "libobs.unmute": [], "libobs.push-to-mute": [], "libobs.push-to-talk": []},
    "deinterlace_mode": 0, "deinterlace_field_order": 0, "monitoring_type": 0,
    "private_settings": {},
}

def item(name, source_uuid, iid):
    return {
        "name": name, "source_uuid": source_uuid, "visible": True, "locked": False,
        "rot": 0.0, "pos": {"x": 0.0, "y": 0.0}, "scale": {"x": 1.0, "y": 1.0},
        "align": 5, "bounds_type": 0, "bounds_align": 0, "bounds_crop": False,
        "bounds": {"x": 0.0, "y": 0.0}, "crop_left": 0, "crop_top": 0,
        "crop_right": 0, "crop_bottom": 0, "id": iid, "group_item_backup": False,
        "scale_filter": "disable", "blend_method": "default", "blend_type": "normal",
        "show_transition": {"duration": 0}, "hide_transition": {"duration": 0},
        "private_settings": {},
    }

scene = {
    "prev_ver": 503316482, "name": "HD60 S", "uuid": scene_uuid,
    "id": "scene", "versioned_id": "scene",
    "settings": {"id_counter": 2, "custom_size": False,
                 "items": [item("Elgato HD60 S", v_uuid, 1),
                           item("Elgato HD60 S Audio", a_uuid, 2)]},
    "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
    "enabled": True, "muted": False, "push-to-mute": False, "push-to-mute-delay": 0,
    "push-to-talk": False, "push-to-talk-delay": 0,
    "hotkeys": {"OBSBasic.SelectScene": []},
    "deinterlace_mode": 0, "deinterlace_field_order": 0, "monitoring_type": 0,
    "private_settings": {},
}

data = {
    "DesktopAudioDevice1": {
        "prev_ver": 503316482, "name": "Desktop Audio", "uuid": desk_uuid,
        "id": "pulse_output_capture", "versioned_id": "pulse_output_capture",
        "settings": {"device_id": "default"}, "mixers": 255, "sync": 0, "flags": 0,
        "volume": 1.0, "balance": 0.5, "enabled": True, "muted": False,
        "push-to-mute": False, "push-to-mute-delay": 0, "push-to-talk": False,
        "push-to-talk-delay": 0,
        "hotkeys": {"libobs.mute": [], "libobs.unmute": [], "libobs.push-to-mute": [], "libobs.push-to-talk": []},
        "deinterlace_mode": 0, "deinterlace_field_order": 0, "monitoring_type": 0,
        "private_settings": {},
    },
    "AuxAudioDevice1": {
        "prev_ver": 503316482, "name": "Mic/Aux", "uuid": mic_uuid,
        "id": "pulse_input_capture", "versioned_id": "pulse_input_capture",
        "settings": {"device_id": "default"}, "mixers": 255, "sync": 0, "flags": 0,
        "volume": 1.0, "balance": 0.5, "enabled": True, "muted": True,
        "push-to-mute": False, "push-to-mute-delay": 0, "push-to-talk": False,
        "push-to-talk-delay": 0,
        "hotkeys": {"libobs.mute": [], "libobs.unmute": [], "libobs.push-to-mute": [], "libobs.push-to-talk": []},
        "deinterlace_mode": 0, "deinterlace_field_order": 0, "monitoring_type": 0,
        "private_settings": {},
    },
    "current_scene": "HD60 S",
    "current_program_scene": "HD60 S",
    "scene_order": [{"name": "HD60 S"}],
    "name": "HD60S",
    "sources": [v4l2, audio, scene],
    "groups": [],
    "quick_transitions": [
        {"name": "Cut", "duration": 300, "hotkeys": [], "id": 1, "fade_to_black": False},
        {"name": "Fade", "duration": 300, "hotkeys": [], "id": 2, "fade_to_black": False},
    ],
    "transitions": [],
    "saved_projectors": [],
    "current_transition": "Fade",
    "transition_duration": 300,
    "preview_locked": False,
    "scaling_enabled": False,
    "scaling_level": 0,
    "scaling_off_x": 0.0,
    "scaling_off_y": 0.0,
    "virtual-camera": {"type2": 3},
    "modules": {"scripts-tool": []},
}
with open(path, "w") as f:
    json.dump(data, f)
print("wrote", path)
PY

cat > "$PROFILE_DIR/basic.ini" <<'INI'
[General]
Name=HD60S

[Video]
BaseCX=1920
BaseCY=1080
OutputCX=1920
OutputCY=1080
FPSType=0
FPSCommon=60
FPSInt=60
FPSNum=60
FPSDen=1
ScaleType=bicubic
ColorFormat=NV12
ColorSpace=709
ColorRange=Partial
INI

mkdir -p "$OBS_HOME"
if [ ! -f "$GLOBAL" ]; then
  cat > "$GLOBAL" <<'INI'
[General]
FirstRun=true

[Basic]
Profile=HD60S
ProfileDir=HD60S
SceneCollection=HD60S
SceneCollectionFile=HD60S
INI
else
  python3 - "$GLOBAL" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
text = p.read_text()
lines = text.splitlines()
out = []
seen_basic = False
keys = {
    "Profile": "HD60S",
    "ProfileDir": "HD60S",
    "SceneCollection": "HD60S",
    "SceneCollectionFile": "HD60S",
}
in_basic = False
replaced = set()
for line in lines:
    if line.strip() == "[Basic]":
        in_basic = True
        seen_basic = True
        out.append(line)
        continue
    if line.startswith("[") and line.strip() != "[Basic]":
        if in_basic:
            for k, v in keys.items():
                if k not in replaced:
                    out.append(f"{k}={v}")
                    replaced.add(k)
        in_basic = False
    if in_basic and "=" in line:
        k = line.split("=", 1)[0]
        if k in keys:
            out.append(f"{k}={keys[k]}")
            replaced.add(k)
            continue
    out.append(line)
if in_basic:
    for k, v in keys.items():
        if k not in replaced:
            out.append(f"{k}={v}")
elif not seen_basic:
    out.append("[Basic]")
    for k, v in keys.items():
        out.append(f"{k}={v}")
p.write_text("\n".join(out) + "\n")
PY
fi

say "OBS: colección HD60S, 1920x1080 @ 60 fps, V4L2 /dev/video42 + hd60s_capture"

if [ "$LAUNCH" -eq 0 ]; then
  exit 0
fi

if ! command -v obs >/dev/null 2>&1; then
  say "aviso: OBS no está en PATH. Instálalo con ./install.sh --extras"
  exit 0
fi

if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
  export DISPLAY="${DISPLAY:-:0}"
fi

if pgrep -x obs >/dev/null 2>&1; then
  say "cerrando OBS para cargar el perfil HD60S..."
  pkill -x obs || true
  sleep 1
  pkill -9 -x obs 2>/dev/null || true
  sleep 0.5
fi

say "abriendo OBS..."
nohup obs >/tmp/hd60s-obs.log 2>&1 &
disown || true
