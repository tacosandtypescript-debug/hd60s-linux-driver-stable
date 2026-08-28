# ---------- helpers ----------
say()  { echo "[hd60s] $*"; }
err()  { echo "[hd60s ERR] $*" >&2; }
die()  { err "$*"; exit 1; }

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "$1 が入ってないっぽい。'hd60s install-deps' で入れる or 'sudo apt install -y $2'"
}

need_sudo() {
    # sudo が使えて、パスワードキャッシュされてるか / 通せるか一応確認
    command -v sudo >/dev/null 2>&1 || die "sudo が要る"
    sudo -n true 2>/dev/null || {
        say "sudo パスワード要求される(先に済ませとくと快適):"
        sudo -v || die "sudo 認証失敗"
    }
}

ensure_iso_capture() {
    [ -x ./iso_capture ] || die "./iso_capture がビルドされてない。'make' 走らせて"
}

ensure_snd_aloop() {
    if ! lsmod | grep -q snd_aloop; then
        say "snd-aloop ロード..."
        sudo modprobe snd-aloop enable=1 index=10 id=hd60s pcm_substreams=1 || die "snd-aloop ロード失敗"
        sleep 1
    fi
}

ensure_v4l2loopback() {
    if ! lsmod | grep -q v4l2loopback; then
        say "v4l2loopback ロード..."
        sudo modprobe v4l2loopback video_nr=42 card_label="HD60S" exclusive_caps=0 || \
            die "v4l2loopback ロード失敗。'hd60s install-deps' で v4l2loopback-dkms 入れて"
        sleep 1
    fi
    [ -e "$VDEV" ] || die "$VDEV が無い。v4l2loopback の video_nr=42 で作られる想定"
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
    die "HD60 S (0fd9:005e) が USB に見つからない。ケーブル刺さってる? 電源入ってる?"
}

set_caps() {
    sudo v4l2loopback-ctl set-caps "$VDEV" "YUYV:1920x1080@60/1" >/dev/null 2>&1 || \
        die "v4l2loopback-ctl set-caps 失敗 ($VDEV)"
}

kill_capture() {
    sudo pkill -TERM -f 'run-hd60s-obs\.sh' 2>/dev/null || true
    sudo pkill -TERM -x iso_capture 2>/dev/null || true
    sleep 1
    sudo pkill -9 -f 'run-hd60s-obs\.sh' 2>/dev/null || true
    sudo pkill -9 -x iso_capture 2>/dev/null || true
}

# 動作準備一括
prep() {
    ensure_iso_capture
    ensure_hd60s_device
    need_sudo
    ensure_v4l2loopback
    ensure_snd_aloop
    set_caps
}
