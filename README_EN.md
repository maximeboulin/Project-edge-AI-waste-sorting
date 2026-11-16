# AI Object Recognition for Waste Sorting Assistance

Embedded AI project — CentraleSupélec, summer 2026.

A **Seeed XIAO ESP32-S3 Sense** locally runs a **FOMO** object detection model
(Edge Impulse) to recognise three categories of waste, indicate the sorting
instruction, and stream the annotated video feed to a web page over Wi-Fi.

**Detected classes:** `bouteille` (bottle) · `canette` (can) · `carton` (cardboard)

**Team:** Ali Ahjyage, Maxime Boulin, Philippe Robin, Hugo Amiel, Tom Aunis
**Supervisor:** Mohamad Mroue

---

## Repository Contents

```
Project_edge_AI_waste_sorting/
├── 1 - Hand_made_dataset/
│   ├── 1 - Bottles (plastic)/              Self-collected images, plastic bottles
│   ├── 2 - Cans/                           Self-collected images, cans
│   ├── 3 - Cardboard/                      Self-collected images, cardboard
│   ├── 4 - Backgrounds/                    Empty-background images (no annotation)
│   └── 5 - Not used/                       Images excluded from the final dataset
│
├── 2 - Arduino_IDE_code/
│   └── 1 - sketch tests/
│       ├── sketch_aug2a/
│       │   └── sketch_aug2a.ino            Early board / camera test sketch
│       └── Stream_webcam_local/
│           └── Stream_webcam_local.ino     Local webcam streaming test
│
├── 3 - Edge_impulse_code/
│   ├── firmware_esp32s3_http_stream.ino    Retained firmware (demonstration)
│   ├── firmware_esp32s3_https.ino          TLS + authentication version (discarded)
│   ├── fomo_train.py                       Edge Impulse training block (expert mode)
│   └── page_web.h                          HTML/JS page served by the board
│
├── Web/
│   ├── firmware_esp32s3_http_stream/
│   │   ├── firmware_esp32s3_http_stream.ino    Retained firmware, ready to open in the Arduino IDE
│   │   └── page_web.h                          HTML/JS page served by the board
│   ├── firmware_esp32s3_https/
│   │   └── firmware_esp32s3_https.ino          TLS + authentication version (discarded)
│   └── ves/                                    <to be described>
│
├── Project_Report.pdf                      Final project report
└── README.md
```

How the folders relate to each other:

- **`1 - Hand_made_dataset/`** holds the images collected by the team following the
  collection protocol described in the report. `5 - Not used/` keeps the images that
  were removed during dataset cleaning (low background/object contrast, inconsistent
  annotations).
- **`2 - Arduino_IDE_code/`** contains the early test sketches used to get familiar
  with the board and the camera, before the final firmware.
- **`3 - Edge_impulse_code/`** is the reference copy of the firmware, the web page and
  the training block.
- **`Web/`** contains the same firmware organised so that it opens directly in the
  Arduino IDE. This is the folder to use to compile and upload.

Two organisational constraints explain the layout of `Web/`:

- **The sketch folder has the same name as the `.ino`**, an Arduino IDE requirement.
- **`page_web.h` stays next to the `.ino`.** The page is isolated in a `.h` file
  on purpose: the IDE's preprocessor only rewrites `.ino` files and breaks large
  raw strings `R"(...)"`.

The Arduino library exported from Edge Impulse is not versioned: it weighs several
megabytes and can be regenerated from the Studio.

---

## Environment

| Item | Version / value |
|---|---|
| Arduino IDE | 2.x |
| ESP32 package (Espressif) | **2.0.17** — 3.x versions break Edge Impulse compatibility |
| Board | `XIAO_ESP32S3` |
| PSRAM | `OPI PSRAM` — **mandatory** |
| Partition scheme | Huge APP (3 MB) |

Board manager URL, to be added in *File > Preferences*:

```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

**External dependencies** — none for the HTTP version: the `esp_http_server` server
is built into the ESP32 core. The HTTPS version, however, requires the
`esp32_https_server` library (Frank Hessel), or its fork `esp32_https_server_compat`
for a 3.x core.

---

## Reproducing the Project

### 1. Get the model

Edge Impulse project: `<link to be completed>`

In the Studio: **Deployment → Arduino library → Build**, which produces a `.zip`.

### 2. Install the library

*Sketch → Include Library → Add .ZIP Library*, then select the downloaded `.zip`.

The header name depends on the Edge Impulse project name; the `#include` line must
be adapted accordingly:

```cpp
#include <Projet_t__inferencing.h>
```

### 3. Configure the credentials

Open `Web/firmware_esp32s3_http_stream/firmware_esp32s3_http_stream.ino` and edit:

```cpp
const char *IDENT_USER = "admin";
const char *IDENT_PASS = "tri";
const char *AP_SSID    = "TriSelectif";
const char *AP_PASS    = "tri12345";   // 8 characters minimum
```

These are demonstration values, written in clear text in the firmware. See the
*Security* section.

### 4. Compile and upload

Check board, port, PSRAM and partition, then upload. Start-up is almost
immediate.

### 5. Use the device

1. Connect to the Wi-Fi network `TriSelectif`
2. Open `http://192.168.4.1/`
3. Log in with the credentials above
4. The annotated stream and the sorting instruction are displayed

---

## Architecture of the Retained Firmware

Three design choices structure this version.

**MJPEG stream over a persistent connection.** The video stream is served as
`multipart/x-mixed-replace` by a **second server, on port 81**. The board pushes
the images over a single connection, instead of going through a connection setup for
each image as with a `/capture` route polled in a loop.

**Inference decoupled from the web server.** It runs continuously in a FreeRTOS task
pinned to **core 0**, while the web server stays on **core 1**. The `/detect` route
no longer computes anything: it returns the latest cached result and responds in a few
milliseconds.

**Centroid merging.** FOMO can produce several centroids for a single large object.
Centroids of the same class closer than `MERGE_DIST_PX` (24 px) are merged on the
firmware side. The stability vote over `N_STABLE` frames, for its part, is done on the
web page side.

### Routes

| Route | Port | Method | Response |
|---|---|---|---|
| `/` | 80 | GET | Interface page |
| `/login` | 80 | POST | `{"token", "ttl"}` or `401` |
| `/detect` | 80 | GET | JSON of the latest result — Bearer token required |
| `/stream` | **81** | GET | MJPEG stream — token as `?token=` parameter |

Detection format:

```json
{
  "detections": [
    { "label": "bouteille", "conf": 0.87, "x": 40, "y": 56 }
  ],
  "timing": { "dsp": 12, "classification": 143 },
  "fps": 4.2
}
```

`x` and `y` denote the centroid in the model's coordinate system. FOMO predicts **no
object dimension**: the page applies the same centred crop as the firmware before
drawing the markers (function `modeleVersAffichage`). The constants `MODEL_W` and
`MODEL_H` in `page_web.h` must therefore equal the actual input resolution of the
deployed model, otherwise the markers are offset.

---

## Model Configuration

| Parameter | Value |
|---|---|
| Colour depth | RGB |
| Architecture | FOMO MobileNetV2, `alpha` = 0.35 |
| Learning rate | 0.001 |
| Epochs | 60 |
| `object_weight` | 250 |
| Quantisation | int8 |

**Expert mode pitfall.** In `3 - Edge_impulse_code/fomo_train.py`, `alpha` and
`object_weight` are **hard-coded** in the `train()` call, unlike `epochs` and
`learning_rate`, which are read from the interface. Once in expert mode, changing
these two fields in the Studio therefore has **no effect**: the code must be edited.

---

## Security: Status and Limits

**Token authentication is active** in the retained version: login screen,
`POST /login`, Bearer token presented on every request, 15-minute validity period,
maximum of 4 simultaneous sessions.

What was removed is **only transport encryption**. A complete HTTPS version
(`Web/firmware_esp32s3_https/`) was developed and then discarded: the TLS handshake
cost around 0.5 to 1 s of computation per image, caused `SSL_accept failed` errors,
and generating the self-signed certificate brought start-up to 30–60 s. This version
is delivered with `USE_EDGE_IMPULSE 0`, and therefore without inference.

**Acknowledged limits:**

- The token travels in clear text. Effective protection relies on the WPA2 encryption
  of the radio link — since the board is itself the access point, there is no network
  segment beyond it. However, anyone holding the Wi-Fi password can read the traffic.
- On `/stream`, the token is passed as a **URL parameter**, since an `<img>` tag
  cannot carry an `Authorization` header. A token in a URL ends up in logs and in the
  browser history.
- The credentials are written in clear text in the firmware. Anyone with physical
  access to the board can read the flash memory.
- A single account, no limit on the number of login attempts.

---

## Known Issues

- The serial port does not always appear at upload: holding the `BOOT` button down
  while plugging in forces bootloader mode.
- The board list is not updated after installing the ESP32 package: restart the IDE.
- The `carton` (cardboard) class is less reliable than the other two — insufficient
  background/object contrast in part of the dataset.

---

## Documentation

The full project report is available in `Project_Report.pdf`.

## References

- Edge Impulse documentation — <https://docs.edgeimpulse.com/>
- Seeed XIAO ESP32-S3 documentation — <https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/>
- Espressif documentation — <https://docs.espressif.com>
- *Low-Power IoT Communication Security: On the Performance of DTLS and TLS 1.3*,
  arXiv:2011.12035 — <https://arxiv.org/abs/2011.12035>
