# ---------- helpers ----------
say()  { echo "[hd60s] $*"; }
err()  { echo "[hd60s ERR] $*" >&2; }
die()  { err "$*"; exit 1; }

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "no encuentro $1. Prueba 'hd60s install-deps' o 'sudo apt install -y $2'"
}

need_sudo() {
    # comprueba que sudo existe y que hay caché de contraseña (o se puede autenticar)
    command -v sudo >/dev/null 2>&1 || die "hace falta sudo"
    sudo -n true 2>/dev/null || {
        say "sudo va a pedir contraseña (mejor adelantarlo):"
        sudo -v || die "falló la autenticación de sudo"
    }
}

ensure_iso_capture() {
    [ -x ./iso_capture ] || die "./iso_capture no está compilado. Ejecuta 'make'"
}

ensure_snd_aloop() {
    if ! lsmod | grep -q snd_aloop; then
        say "cargando snd-aloop..."
        sudo modprobe snd-aloop enable=1 index=10 id=hd60s pcm_substreams=1 || die "falló al cargar snd-aloop"
        sleep 1
    fi
}

ensure_v4l2loopback() {
    if ! lsmod | grep -q v4l2loopback; then
        say "cargando v4l2loopback..."
        sudo modprobe v4l2loopback video_nr=42 card_label="HD60S" exclusive_caps=0 || \
            die "falló al cargar v4l2loopback. Instala v4l2loopback-dkms con 'hd60s install-deps'"
        sleep 1
    fi
    [ -e "$VDEV" ] || die "no existe $VDEV. v4l2loopback debería crearlo con video_nr=42"
}

usb_device_present() {
    for usb in /sys/bus/usb/devices/*; do
        [ -r "$usb/idVendor" ] || continue
        [ "$(cat "$usb/idVendor" 2>/dev/null)" = "0fd9" ] || continue
        [ "$(cat "$usb/idProduct" 2>/dev/null)" = "005e" ] && return 0
    done
    command -v lsusb >/dev/null 2>&1 || return 1
    lsusb 2>/dev/null | grep -qi "0fd9:005e"
}

ensure_hd60s_device() {
    # HD60 S: VID 0fd9, PID 005e (the device supported by iso_capture).
    usb_device_present && return 0
    die "no encuentro el HD60 S (0fd9:005e) en USB. ¿Cable enchufado? ¿Tiene alimentación?"
}

set_caps() {
    sudo v4l2loopback-ctl set-caps "$VDEV" "YUYV:1920x1080@60/1" >/dev/null 2>&1 || \
        die "falló v4l2loopback-ctl set-caps ($VDEV)"
}

kill_capture() {
    sudo pkill -TERM -f 'run-hd60s-obs\.sh' 2>/dev/null || true
    sudo pkill -TERM -x iso_capture 2>/dev/null || true
    sleep 1
    sudo pkill -9 -f 'run-hd60s-obs\.sh' 2>/dev/null || true
    sudo pkill -9 -x iso_capture 2>/dev/null || true
}

# preparación de runtime de una vez
prep() {
    ensure_iso_capture
    ensure_hd60s_device
    need_sudo
    ensure_v4l2loopback
    ensure_snd_aloop
    set_caps
}
