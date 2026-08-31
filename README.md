# HD60 S Linux Driver

Driver **instalable** para la **Elgato Game Capture HD60 S** en Linux: **1080p60**, audio HDMI y passthrough al televisor.

No hay driver oficial fuera de Windows y macOS. Este proyecto habla el USB del aparato y deja una cámara V4L2 + audio PipeWire para OBS.

Solo el modelo **VID `0fd9` / PID `005e`**. No vale para HD60 S+ ni HD60 X.

![Fortnite en OBS con el HD60 S (1080p60)](docs/screenshot.png)

*Captura en OBS: Fortnite por **Dispositivo de captura de video (V4L2)** y audio **Elgato HD60 S**, 1920×1080 a 60 fps.*

## Instalar (un comando)

Enchufa el HD60 S a **USB 3.0**, con alimentación, y en una terminal:

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/tacosandtypescript-debug/hd60s-linux-driver-stable/main/get.sh)
```

Eso instala el driver, configura OBS a **1920×1080 @ 60 fps** (vídeo **ElgatoHD60S** + audio **hd60s_capture**) y **abre OBS**.

Usa `bash <(curl …)` — no `curl | bash` — para que sudo pueda pedir la contraseña.

Requisitos: Linux (Mint / Ubuntu / Fedora / Arch), **USB 3.0**, **PipeWire**, sudo.

## Quitar

```bash
cd ~/hd60s-linux-driver-stable
./uninstall.sh
```

## Checkout a mano

```bash
git clone https://github.com/tacosandtypescript-debug/hd60s-linux-driver-stable.git
cd hd60s-linux-driver-stable
./install.sh --extras --open-obs
```

En kernels nuevos (p. ej. 7.0 HWE) el `v4l2loopback-dkms` 0.12.7 de Ubuntu no compila. El instalador usa el módulo que ya tengas o, si falta, compila [v4l2loopback 0.15.4](https://github.com/v4l2loopback/v4l2loopback).

## Uso en OBS

Si ya instalaste con `get.sh`, OBS debería abrir solo con las fuentes listas.

Si no:

1. Enchufa el HD60 S a **USB 3.0** y dale alimentación.
2. `hd60s open-obs`  (o abre OBS a mano).
3. Fuentes:
   - **Dispositivo de captura de video (V4L2)** → **ElgatoHD60S** (`/dev/video42`), 1920×1080, 60 fps, YUYV.
   - Audio: **hd60s_capture** (PipeWire, 48 kHz estéreo).

La captura arranca al iniciar sesión y al enchufar el USB.

```text
hd60s doctor     comprueba módulos, USB, /dev/video42 y el servicio
hd60s open-obs   perfil 1080p60 + fuentes y abre OBS
hd60s stop       para la captura
hd60s help       resto de comandos
```

Si el instalador te acaba de meter en el grupo `video`, cierra sesión y vuelve a entrar.

## Qué deja instalado

- Vídeo **1080p60 YUYV** en `/dev/video42` (cámara **ElgatoHD60S**).
- Audio **48 kHz estéreo** en PipeWire como `hd60s_capture` (sink permanente `hd60s_out`; no se pierde si se reinicia la captura).
- Perfil OBS **HD60S** (lienzo 1920×1080 @ 60).
- **Passthrough HDMI** (HDMI OUT → TV) a la vez que la captura.
- Servicio de usuario `hd60s`: arranca al iniciar sesión y al enchufar el USB.

Comprobado con Nintendo Switch.

## Requisitos

- HD60 S (`0fd9:005e`), no S+ / X.
- USB 3.0. 1080p60 pide ~2 Gbps; USB 2.0 no alcanza.
- PipeWire (no PulseAudio a secas).
- Grupo `video` para leer `/dev/video42`.

## Qué no es

- No es un módulo del kernel propio. Corre en userspace con libusb y usa v4l2loopback para que OBS vea una cámara.
- No hay 720p ni 30 fps: solo 1080p60.
- Proyecto no oficial, sin relación con Elgato Systems GmbH.

## Compilar a mano

```bash
./scripts/hd60s install-deps
make all
sudo make install
systemctl --user daemon-reload
systemctl --user enable --now hd60s
hd60s open-obs
```

`live` / `obs` / `start` son de laboratorio y pueden chocar con el servicio. Para el día a día basta `get.sh` o `hd60s open-obs`.

## Limitaciones

- 1080p60 YUYV sin comprimir (~2 Gbps): `iso_capture` y OBS usan CPU de verdad.
- El loopback va con `exclusive_caps=1`. Con 0, OBS 30 puede abortar al listar el dispositivo.
- OBS 30 de Ubuntu hace `scandir("/dev/v4l/by-id")`; si el directorio no existe, abrir propiedades aborta. `./install.sh` crea el directorio y el enlace a `video42`.
- El loopback anuncia 30 fps en el lado CAPTURE si no se fuerza; el instalador y el servicio fijan `--set-parm=60` para que OBS negocie 1080p60.
- No hay hotplug por libusb; el supervisor espera a que el USB vuelva a enumerar.

## Licencia

[MIT](LICENSE). Copyright (c) 2026 [tacosandtypescript-debug](https://github.com/tacosandtypescript-debug).
