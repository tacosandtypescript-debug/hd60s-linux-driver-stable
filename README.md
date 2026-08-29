# HD60 S Linux Driver

Driver en espacio de usuario para la tarjeta de captura HDMI **Elgato HD60 S** en Linux.

No hay driver oficial fuera de Windows y macOS. Este proyecto habla el protocolo USB del aparato, saca vídeo 1080p60 y audio, y deja el HDMI passthrough hacia el televisor para jugar con poca latencia.

Dispositivo soportado: **VID `0fd9` / PID `005e`**. No es el HD60 S+ ni el HD60 X.

![Partida de Splatoon 3 capturada con el HD60 S](docs/screenshot.png)

*Salida de `iso_capture` en `/dev/video42` (1080p60 YUYV).*

## Instalación

Hace falta **USB 3.0**, **PipeWire** y el HD60 S (`0fd9:005e`). Probado en Linux Mint / Ubuntu. También hay paquetes para Fedora (dnf) y Arch (pacman).

En kernels nuevos (p. ej. 7.0 HWE) el `v4l2loopback-dkms` de Ubuntu 0.12.7 no compila. `./install.sh` usa el módulo que ya tengas o, si falta, compila [v4l2loopback 0.15.4](https://github.com/v4l2loopback/v4l2loopback).

```bash
git clone https://github.com/tacosandtypescript-debug/hd60s-linux-driver-stable.git
cd hd60s-linux-driver-stable
./install.sh
```

El script pide sudo, instala las dependencias, compila, configura udev / v4l2loopback / WirePlumber y activa el servicio de usuario `hd60s`. Si también quieres OBS, mpv y tmux:

```bash
./install.sh --extras
```

Quitar:

```bash
./uninstall.sh
```

## Uso en OBS

1. Enchufa el HD60 S a un puerto **USB 3.0** y dale alimentación.
2. Abre OBS.
3. Fuentes:
   - **Dispositivo de captura de video (V4L2)** → **ElgatoHD60S** (`/dev/video42`), 1920×1080, 60 fps, YUYV.
   - Audio: **hd60s_capture** (PipeWire, 48 kHz estéreo).

La captura arranca al iniciar sesión y al enchufar el USB (`systemctl --user enable --now hd60s`).

```text
hd60s doctor     comprueba módulos, USB, /dev/video42 y el servicio
hd60s stop       para iso_capture
hd60s help       resto de comandos (live, obs, experimentos)
```

Si acabas de ser añadido al grupo `video`, cierra sesión y vuelve a entrar.

## Qué hace

Tras `./install.sh`:

- Vídeo **1080p60 YUYV** en `/dev/video42` (v4l2loopback **ElgatoHD60S**, `exclusive_caps=1`).
- Audio **48 kHz estéreo** (PCM HDMI nativo) por PipeWire `hd60s_capture`.
- **Passthrough HDMI** (HDMI OUT del HD60 S → TV) a la vez que la captura USB.
- Reintento automático si se desconecta el USB (`scripts/run-hd60s-obs.sh`).

Comprobado con Nintendo Switch.

## Qué no es

- **HD60 S+** y **HD60 X**: otro chipset. No arrancan con este código.
- Un módulo del kernel. Todo corre en userspace con libusb (sí usa v4l2loopback para que OBS vea una cámara).
- Un comando `hd60s prep`. `prep` es una función interna de `scripts/hd60s-lib.sh`.
- `color-test`: se está retirando y no forma parte del flujo actual.

## Requisitos

- **USB 3.0 (SuperSpeed)**. 1080p60 pide ~2 Gbps; USB 2.0 no alcanza.
- **PipeWire** (no PulseAudio a secas).
- **v4l2loopback** en `/dev/video42` (`video_nr=42`, `exclusive_caps=1`).
- Elgato HD60 S en el bus: `0fd9:005e`.
- Grupo `video` para leer `/dev/video42`.

## Compilar a mano

Desde la raíz del repo, si no quieres `./install.sh`:

```bash
./scripts/hd60s install-deps
make all
sudo make install
systemctl --user daemon-reload
systemctl --user enable --now hd60s
```

`install-deps` instala compilador, libusb, ALSA, PipeWire, libsamplerate, v4l2loopback, headers del kernel y extras (tmux, mpv, VLC, ffmpeg, OBS).

`make install` copia el binario a `/usr/local/libexec/hd60s`, deja `hd60s` en `PATH`, la regla udev `packaging/70-elgato-hd60s.rules`, WirePlumber, el unit `packaging/hd60s.service.in` y los tmpfiles de `/dev/v4l/by-id` (OBS 30 aborta si ese directorio no existe).

## Comandos de laboratorio

Desde el checkout, con `iso_capture` compilado:

```text
./scripts/hd60s live           tmux: supervisor arriba, mpv en /dev/video42 a los 13 s
./scripts/hd60s obs            arranca la captura y abre OBS
./scripts/hd60s start [seg]    iso_capture en primer plano (600 s por defecto)
./scripts/hd60s view           VLC con vídeo y audio
./scripts/hd60s stop           mata iso_capture y la sesión tmux
```

`live` está pensado para desarrollo. Para uso diario basta el servicio + OBS.

En tmux, panel de arriba: `scripts/run-hd60s-obs.sh`. Panel de abajo: mpv con `--vo=wlshm`. Sin preview: `HD60S_NO_MPV=1 ./scripts/hd60s live`.

- Cambiar de panel: `Ctrl+B` y flechas
- Detach: `Ctrl+B`, `d`
- Salir del todo: `Ctrl+B`, `q`, o `./scripts/hd60s stop`

PipeWire nativo (el servicio lo usa): `HD60S_AUDIO_PW=1`. `live` por defecto escribe en snd-aloop (`HD60S_AUDIO_PW=0`) y publica `hd60s_capture` con `pactl`.

`./scripts/hd60s help` lista experimentos (`test`, `minimal`, `view-mpv`, etc.). `pt` / `ptx` están marcados como rotos: no usarlos.

## Estructura

```
install.sh / uninstall.sh  instalador de un comando
src/iso_capture.c          captura (orquesta los módulos)
src/util/                  hd60s_util
src/v4l2/                  hd60s_v4l2
src/audio/                 hd60s_audio
src/replay/                hd60s_replay + includes de audio ISO
src/pace/                  hd60s_pace
src/parser/                hd60s_parser
src/usb/                   hd60s_usb
tools/                     audio_extract, offline_parser, spi_dump, probe_iso
analysis/                  TSV de init/burst
                           (live usa init-p2-audio-fast.tsv y poststream-no9a.tsv)
scripts/hd60s              lanzador
scripts/hd60s-lib.sh       helpers (módulos, USB, doctor)
scripts/hd60s-install.sh   lógica del instalador
scripts/run-hd60s-obs.sh   supervisor: espera el USB y relanza iso_capture
packaging/                 udev, unit systemd, regla WirePlumber, modprobe
docs/                      screenshot.png
Makefile
```

Los binarios (`iso_capture` y las tools) se generan en la raíz del repo. El lanzador vive en `scripts/`; resuelve la raíz del checkout o `/usr/local/libexec/hd60s`.

## Limitaciones

- 1080p60 YUYV sin comprimir (~2 Gbps). `iso_capture` y OBS usan CPU de verdad; no es un fallo del driver.
- Solo 1080p60. No hay 720p ni 30 fps.
- El loopback tiene que ir con `exclusive_caps=1`. Con 0, OBS 30 puede abortar en `linux-v4l2.so`.
- OBS 30 de Ubuntu hace `scandir("/dev/v4l/by-id")`; si el directorio no existe, abrir propiedades aborta. `./install.sh` crea `/dev/v4l/by-id` y un symlink a `video42`.
- Wayland + NVIDIA: mpv con `--vo=gpu-next` puede fallar; `live` fuerza `--vo=wlshm`.
- No hay hotplug por libusb. El supervisor hace polling hasta que el USB vuelve a enumerar.
- Implementación no oficial, sin relación con Elgato Systems GmbH. Uso bajo tu responsabilidad.

## Licencia

[MIT](LICENSE). Copyright (c) 2026 kusq.
