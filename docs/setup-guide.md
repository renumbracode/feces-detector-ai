# Setup guide

End-to-end setup for the Pigpen Feces Detector — **hybrid ESP32-S3 + server
YOLOv8**. There is **no Raspberry Pi**.

Components:

1. **ESP32-S3 + OV3660 edge device** (ESP-IDF) — on-device FOMO inference + auto-spray.
2. **Python YOLOv8 verification server** (FastAPI + ultralytics).
3. **CodeIgniter 3 + MySQL dashboard** (XAMPP) — monitoring + manual override.

## 1. Web dashboard (XAMPP + CodeIgniter 3)

The dashboard lives at `C:\xampp\htdocs\feces-detector-ai\dashboard`:

```
http://localhost/feces-detector-ai/dashboard/
```

Steps:

1. Start **Apache** and **MySQL** from the XAMPP control panel.
2. Import the schema:
   - phpMyAdmin -> Import -> `dashboard/database.sql`, or
   - `C:\xampp\mysql\bin\mysql.exe -u root < dashboard\database.sql`
3. **Add the CodeIgniter 3 `system/` folder** — download CodeIgniter 3 from
   https://codeigniter.com/download and extract `system/` into `dashboard/`
   (see `dashboard/README.md`).
4. Check `dashboard/application/config/database.php` matches your MySQL
   user (XAMPP default: `root`, empty password).
5. Open the dashboard — you should see zero detections.

## 2. Python YOLOv8 verification server

Runs unmodified YOLOv8 on the server (feasible there, not on the MCU).

```bash
cd server/yolo_verify
pip install -r requirements.txt
# set the model path (optional):
#   export YOLO_MODEL=/abs/path/to/training/runs/detect/weights/best.pt
uvicorn app:app --host 0.0.0.0 --port 8000
```

Verify: `curl http://localhost:8000/health` returns model info.

## 3. ESP32-S3 firmware

### Hardware

- ESP32-S3 board with **OV3660** camera (Wi-Fi + BT, PSRAM)
- Alarm buzzer on `APP_PIN_BUZZER` (**GPIO 21**) via a 220 ohm resistor
- External LED on `APP_PIN_LED` (**GPIO 1**) via a 330 ohm resistor
- Optional relay module on `APP_PIN_RELAY` (**GPIO 14**) for a water pump;
  set `relay_active_low` to match your module

Each load needs its **own** pin. The buzzer must not share GPIO14 with the relay —
two GPIO outputs on one pad fight each other, and a relay on a shared pad never
latches cleanly while the log still reports `Spray ON`.

See [wiring-guide.md](wiring-guide.md) for the full pin map and breadboard layout.

### Build & flash

Requires ESP-IDF v5.x. From `firmware/esp32s3_fomo`:

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

See `firmware/esp32s3_fomo/README.md` for the full workflow, including how to
insert your trained FOMO model.

> **Switching Wi-Fi / server URLs at runtime (no reflash):** the ESP serves
> `GET /setup` — open `http://<esp-ip>/setup` in a browser, change SSID,
> password, verify URL, or dashboard URL, and the board saves them to NVS and
> reboots into the new network. Find the board's new IP in the router's
> connected-device list.

## 4. Train the models

### On-device FOMO (ESP32-S3)

```bash
cd training/fomo
python prepare_dataset.py --format yolo-txt
# then follow training/fomo/README.md: upload the two zips to Edge Impulse as
# "YOLO TXT" (160x160, FOMO-MobileNetV2 0.35, learning rate 0.001), train,
# then export the ESP32 C++ library into
# firmware/.../components/inference/edge_impulse/
```

### Server YOLOv8

```bash
cd training
python train.py --model yolov8n --epochs 100
python evaluate.py --model runs/detect/weights/best.pt   # target mAP50 >= 80%
```

## 5. End-to-end test

1. Set the device (`esp32_ip` / `esp32_port`) and `verify_url` in the dashboard **Control** page.
2. Open **Live** and confirm the ESP32 MJPEG stream appears.
3. Click **Spray now** — the pump should run for the chosen duration (manual override).
4. Place a test object where feces would be; verify:
   - Detection is logged in **History** (with a YOLOv8-verified badge when the server agrees),
   - Pump triggers automatically on-device (respecting cooldown),
   - Confidence appears in the **Dashboard** stats.

## 6. Tuning

- **False positives** (sprays on mud/shadow): raise `threshold` on the device
  (e.g. 0.60 → 0.75) or add those images to the dataset and retrain.
- **False negatives**: lower threshold, improve lighting, add more training images.
- **Cooldown**: device NVS `cooldown_ms` (firmware default 0 = no hold-back, so
  every detection above the dial sprays; this board is set to 30000 = 30 s). When a
  cooldown *is* active the pump is held back but the LED and buzzer still alert, so a
  detection is never silently dropped.
- **FPS**: FOMO runs fastest at 96x96 grayscale; use 160x160 for accuracy.

## 7. Mobile hotspot (no router)

The three components do not need a home router. An Android phone hotspot works
as the shared network, as long as **the laptop joins the hotspot too** — the
XAMPP dashboard and the YOLOv8 server run on the laptop, and the ESP32 needs an
L2 path to reach them.

```
[Android phone] ──hotspot AP──┬── [Laptop]  192.168.43.xxx   Apache :80 + uvicorn :8000
                              └── [ESP32-S3] 192.168.43.yyy  2.4 GHz only
```

Android hotspots do not enable client isolation by default, which is what makes
laptop↔ESP32 traffic work.

### 7.1 Pre-flight (before touching the ESP32)

1. **Force the hotspot band to 2.4 GHz.** Some Android/OEM hotspots default to
   5 GHz or Wi-Fi 6E. The ESP32-S3 has no 5 GHz radio and will not see the SSID.
2. **Keep the phone plugged in.** OEM battery managers kill the hotspot after a
   few minutes of screen-off, dropping the ESP32's association with it.
3. **Windows Firewall.** The hotspot NIC is a new interface, normally classified
   as *Public*, so the rules that let Apache and uvicorn through on the home
   network may not apply. Check `wf.msc` -> *Windows Defender Firewall with
   Advanced Security* -> *Inbound Rules* for **Apache HTTP Server (80)** and
   **Python/uvicorn (8000)** and confirm they cover the **Public** profile
   (or are scoped to the hotspot subnet). Note that the board *initiating* a
   connection to the laptop is blocked by a Public-profile default, whereas
   laptop-to-board requests succeed because the reply rides an outbound
   connection the laptop opened itself.
4. **Confirm the verify server is actually listening.** uvicorn does not
   autostart with XAMPP; if port 8000 is silent the board logs connection
   failures and `vrf.valid` stays `false`. Start it from `server/yolo_verify`
   (§2) and confirm `curl http://localhost:8000/health`. `model_loaded: false`
   in that response is fine — the model lazy-loads on the first `/verify`.
5. Confirm `C:\xampp\apache\conf\extra\httpd-xampp.conf` says `Listen 80`
   (not `Listen <lan-ip>:80`), otherwise the dashboard is unreachable from the ESP32.

### 7.2 Point the ESP32 at the hotspot

**This takes two rounds, and it cannot be done in one.** The reason is
circular: the board is only reachable from the network it is *currently* on,
but you need the laptop's hotspot IP to build the URLs, and the laptop only
has a hotspot IP *after* joining the hotspot — at which point it can no longer
reach the board on the home LAN.

So: send the credentials first while the laptop is still on the home LAN, then
send the URLs once the laptop is on the hotspot.

**Round 1 — credentials only** (laptop still on the home LAN):

```bash
curl -X POST http://<OLD_ESP_IP>/config \
  --data-urlencode "wifi_ssid=<hotspot ssid>" \
  --data-urlencode "wifi_pass=<hotspot password>"
```

`handler_config_post` (`components/http_server/http_server_service.c`) only
overwrites a key that is present and non-empty, so omitting the URLs leaves them
at their current values. The board reboots and joins the hotspot. The URLs now
point at an address that is unreachable on the hotspot, so it will log
`Dashboard log failed` and the overlay stays dead — that is expected until
round 2.

**Then:** connect the **laptop** to the hotspot and note its address
(`ipconfig`).

**Round 2 — the real URLs** (laptop now on the hotspot):

```bash
curl -X POST http://<NEW_ESP_IP>/config \
  --data-urlencode "verify_url=http://<LAPTOP_IP>:8000/verify" \
  --data-urlencode "dash_url=http://<LAPTOP_IP>/feces-detector-ai/dashboard/api/insert" \
  --data-urlencode "dash_verify_url=http://<LAPTOP_IP>/feces-detector-ai/dashboard/api/verify"
```

Read `<NEW_ESP_IP>` from the serial log rather than assuming — the board reboots
and DHCP may reassign it. Watch the boot over the existing USB-C cable; USB is
independent of Wi-Fi, so `idf.py monitor` keeps working:

```
wifi_svc: Connecting to SSID: <ssid>
wifi:connected with <ssid>, aid = ..., channel ...
wifi_svc: Got IP: <new ip>
```

The three URL defaults compiled into the firmware
(`components/app_config/include/app_config.h`, `APP_DEFAULT_*_URL`) are
placeholders (`192.168.1.3`), which is why every one of them must change. They
persist in NVS — changing them needs no reflash, and a reflash will not reset
them either.

Verify with `curl http://<NEW_ESP_IP>/status`: `vrf.valid` should turn `true`
once the board can reach the verify server.

### 7.3 Point the dashboard at the ESP32

Dashboard -> **Control** -> Settings:

- `esp32_ip` = the ESP32's hotspot IP; leave `esp_port` at `80`.
- **`verify_url` stays `http://localhost:8000/verify`.** That URL is consumed by
  the PHP process running on the laptop (`application/core/MY_Controller.php`,
  `verify_url()`), so `localhost` is still correct from the hotspot.
- Clear `stream_url` if it still holds a stale `192.168.1.x` value.

### 7.4 What to expect

- **The ESP32's IP changes on every hotspot session.** DHCP is used, there is no
  static IP, and mDNS is off (`sdkconfig.defaults`,
  `CONFIG_ESP_HTTP_SERVER_ENABLE_MDNS=n`). Re-read it from the phone's
  connected-device list, or `arp -a` / a ping sweep from the laptop, and re-enter
  `esp32_ip`. Android hotspots do not reliably forward mDNS multicast between
  clients, so enabling mDNS would not fix this.
- **View the dashboard from the laptop only.** `views/live/index.php` puts the
  device URL in a raw `<img src>` and `assets/js/main.js` fetches `/status`
  directly from the browser. A phone browser cannot reach its *own* hotspot
  clients, so stream and status tiles will be blank on the phone even while the
  backend is receiving data correctly.
- **The verifier overlay may disappear for ~30 s at a time.**
  `prv_poll_verify` runs inline in the ~10 fps detect loop with a 1.5 s timeout
  (`components/detection_service/detection_service.c`); one missed reply sets
  `s_verify_ok = false` and imposes a 30 s backoff. A phone hotspot is noisier
  than a wired router, so misses are common. This is display-only — auto-spray
  and DB rows are unaffected.
- **`wifi_service.c` reconnects with no backoff**, so a dropped hotspot makes the
  board spin on `esp_wifi_connect()`. Harmless for a demo, not for unattended use.
- **`WIFI_AUTH_WPA2_PSK` is hardcoded.** A WPA3-only or WPA2/WPA3-mixed hotspot
  will fail to associate, and the 20 s boot wait swallows the error (it returns
  `ESP_OK` regardless), so the board just looks dead on the network.

### 7.5 Recovering a board that no longer has a reachable network

If round 1 is sent with an SSID that does not exist (a typo, or a hotspot that
was renamed before the board rebooted), the board leaves the network it was
reachable on and retries a name that will never appear. It is then
**unreachable and unfixable over HTTP**, because the only code that writes NVS
is `app_config_save` (`components/app_config/app_config.c`) and its only caller
is `POST /config`. There is no serial console, REPL, or BLE provisioning in this
firmware.

The recovery is to rewrite the NVS partition directly over USB. This needs no
source change and no rebuild — only the `nvs` partition is rewritten, leaving
the app untouched.

The `nvs` partition lives at **0x9000**, size **0x6000**. (That address comes
from ESP-IDF's built-in single-app partition table, **not** from the repo's
`partitions.csv` — see the note in `sdkconfig.defaults`. The `storage` fat
partition listed in that csv does not exist on the board.) Write a CSV
**outside the repo** so the password is never committed:

```
key,type,encoding,value
fomo_app,namespace,,
wifi_ssid,data,string,"<ssid>"
wifi_pass,data,string,"<password>"
```

Then generate and flash it:

```bash
PY=/c/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe
$PY -m esp_idf_nvs_partition_gen generate wifi.csv wifi_nvs.bin 0x6000
$PY -m esptool --chip esp32s3 --port COMx write_flash 0x9000 wifi_nvs.bin
```

Only `wifi_ssid`/`wifi_pass` are included, which is deliberate:
`app_config_load` seeds the compiled defaults first and then overrides per key,
so absent keys fall back to `APP_DEFAULT_*_URL` and the tuning defaults. This
gives you a board on the new SSID with round 2 still to do. If esptool reports
`Failed to connect`, hold **BOOT**, tap **RESET**, release **BOOT**, retry.

This is also the easiest way to go **back** to a previous network, and unlike
`POST /config` it needs no network path to the device at all.

#### Writing the whole config in one flash (no round 2)

Two rounds are only needed because round 2 requires reaching the board. If you
already know the laptop's address on the new network, skip that by writing every
key at once. The CSV is longer, and three entries are easy to get wrong:

```
key,type,encoding,value
fomo_app,namespace,,
wifi_ssid,data,string,"<ssid>"
wifi_pass,data,string,"<password>"
verify_url,data,string,http://<LAPTOP_IP>:8000/verify
dash_url,data,string,http://<LAPTOP_IP>/feces-detector-ai/dashboard/api/insert
dash_verify_url,data,string,http://<LAPTOP_IP>/feces-detector-ai/dashboard/api/verify
static_ip,data,string,<ESP_IP>
static_mask,data,string,255.255.255.0
static_gw,data,string,<ROUTER_IP>
threshold,data,hex2bin,9a99193f
srv_trig_conf,data,hex2bin,295c8f3e
cooldown_ms,data,u32,30000
spray_ms,data,u32,5000
relay_act_low,data,u8,1
```

- **`threshold` and `srv_trig_conf` are `hex2bin`, not `u32`.** They are stored
  as a raw 4-byte `float` blob (`app_config.c:167,171`), so they must be
  little-endian. `0.60` = `9a99193f`, `0.28` = `295c8f3e`, `0.75` = `0000403f`.
  Writing `0.60` as a `u32` does not error — it silently leaves
  `APP_DEFAULT_THRESHOLD` standing. Compute with
  `python -c "import struct;print(struct.pack('<f',0.60).hex())"`.
- **Write all three `static_*` keys, or none.** `prv_load_static`
  (`app_config.c:83`) discards an incomplete set and falls back to DHCP, leaving
  the board at an address you have to go and discover.
- Keys are capped at **15 characters** (`app_config.h:13`), which is why the
  constants are `relay_act_low` and `srv_trig_conf` rather than the readable
  `relay_active_low` / `server_trigger_conf`.

Static addressing changes what the boot log looks like: `GOT_IP` never fires
because the DHCP client is stopped, so the success lines are
`app_cfg: Static IP configured: <ip>/<mask> gw <gw>` and
`wifi_svc: WiFi connected`. `wifi_svc: AP unreachable after 30 s` means the
credentials or WPA mode are wrong — and because detection and spray keep running
offline, the board still looks alive, so do not assume "it is working" from the
dashboard alone.

Verify with `GET /config`, which echoes back every key in the CSV **except**
`wifi_ssid`/`wifi_pass`. That read-back is the only confirmation that the
tuning values landed: they are what you typed, not what the board was running
before the rewrite.

> **You cannot read the saved Wi-Fi credentials back off the board.**
> `GET /config` returns only the tuning values and the three URLs — not
> `wifi_ssid` or `wifi_pass` — so once you overwrite them the previous password
> is gone from the device. Note it down before you overwrite it.

### 7.6 Known gaps in the firmware (not implemented)

- **Soft-AP fallback is documented but does not exist.** `main/esp32s3_fomo_main.c`
  and `components/wifi_service/include/wifi_service.h` both describe starting a
  soft-AP when the network is unreachable; `wifi_service.c` only ever calls
  `esp_wifi_set_mode(WIFI_MODE_STA)`. On an unreachable AP the board boots
  headless and cannot be reached at all — which is why §7.2 must be done while
  the board is still on a network you can reach, and why §7.5 exists.
- **`WIFI_FAIL_BIT` is dead code.** Declared and waited on, never set by any handler.
- **Credentials cannot be read back.** `GET /config` omits `wifi_ssid`/`wifi_pass`,
  so the only record of the network a board is on is whatever you wrote down.

## 8. Two-board setup (camera board + actuator board)

The system runs as **two ESP32 boards on the same Wi-Fi**:

| Board | Firmware | Role |
|---|---|---|
| Camera ESP32-S3 | `firmware/esp32s3_fomo` | camera, FOMO detection, YOLOv8 verify poll, **publishes coordinates**, its own legacy local spray path |
| Main ESP32 (actuator) | `firmware/esp32_main` | 2 servos, pump relay, water-level sensor, coordinate→angle conversion, the cleaning cycle |

The camera board detects and publishes; the main board decides whether it can
act. The wire contract is `docs/target-protocol.md`; the main board's pins
are in `docs/wiring-guide.md`.

### 8.1 Build and flash the main board

```bash
scripts\esp_idf_build_main.cmd build
scripts\esp_idf_build_main.cmd -p COMx flash
scripts\esp_idf_build_main.cmd -p COMx monitor
```

The main board's HTTP server listens on **8080**.

### 8.2 Give the main board its Wi-Fi and its calibration

Over USB (the board has no stored credentials yet, so it cannot be reached
over HTTP):

```
http://<main-board-ip>:8080/setup
```

Set the Wi-Fi SSID/password of the **same network the camera board is on**,
then the calibration:

- `cam_x`, `cam_y` — the camera's position on the pen floor, in mm
- `cam_height` — how high the camera is above the floor
- `cam_pitch` — how far below horizontal it looks
- `cam_yaw` — 0 when the camera looks toward +Y
- `focal_px` — the focal length in pixels, or derive it from the field of view
- `nozzle_x`, `nozzle_y`, `nozzle_h` — the nozzle mount
- `pan_min/max`, `tilt_min/max` — the servo's **mechanical** travel
- `water_min` — the level below which cleaning is refused

The page says so too; it is the same list.

### 8.3 Check the geometry before anything moves

```bash
curl http://<main-board-ip>:8080/selftest
```

`{"ok":true,"passed":N,"failed":0}` means the solver itself is correct. If it
fails, the numbers in the calibration are not the problem — the maths is, and
the message says which check failed. This is the first thing to run when the
nozzle aims somewhere unexpected: it separates "the maths is wrong" from "the
measured numbers are wrong".

### 8.4 Point the camera board at the main board

From the dashboard: **Control → Actuator board (main ESP32)**, enter the URL
and switch publishing on. That is applied to the camera board **immediately,
with no reboot**.

Or by hand:

```bash
curl "http://<camera-board-ip>/target?url=http://<main-ip>:8080/target&enable=1"
```

### 8.5 Switch the camera board's own pump OFF

**This is not optional.** The camera board keeps its own relay and its own
local spray path (it is left unchanged so it can be reused for something
else). While the main board is also connected, both would spray for one
detection.

```bash
curl -X POST http://<camera-board-ip>/config -d 'spray_enabled=0'
```

The camera board then still detects, still alerts, still logs and still
publishes — it just refuses to energise its own relay. The same switch is on
the Control page and on the board's `/setup` page.

### 8.6 The staged test procedure

Work through these in order. **Do not connect the water until step 5.**

**Stage 1 — coordinates only, nothing moves.**
The main board must log the received message. With publishing on and the
camera board pointed at it, the main board's serial log should show
`detection N -> accepted ...` or a refusal reason. If nothing arrives, the
two boards are not on the same network, or the URL is wrong.

```bash
curl http://<main-board-ip>:8080/status
```

**Stage 2 — servo positioning, pump still disabled.**
Move the nozzle by hand and watch it. The main board's `/servo` endpoint
**never touches the pump** — that separation is the point:

```bash
curl "http://<main-board-ip>:8080/servo?pan=0&tilt=30"
```

Then check the geometry against a target placed at a **known floor
position**: put something on the floor at a measured spot, make the camera
see it, and confirm the nozzle aims there. This is where a wrong `cam_pitch`
or a wrong `focal_px` shows up. Fix the calibration and repeat until it
lands right.

**Stage 3 — the relay alone, no water.**
With the pump's water disconnected:

```bash
curl "http://<main-board-ip>:8080/pump?duration=1000"
```

The relay should click for one second and the pump should turn. **Watch for
a reset during the spray** — that is the brownout symptom of the pump
sharing the board's supply, not a firmware bug.

**Stage 4 — the pump alone, with water.**
Short runs, and confirm the low-water protection. Empty the tank (or set
`water_min` above the current level) and check that the pump is **refused**
with `low_water`, not merely that it runs briefly:

```bash
curl "http://<main-board-ip>:8080/pump?duration=1000"
```

**Stage 5 — the complete cleaning cycle.**
Both boards connected, the camera board's own pump off, water present. Show
feces to the camera and watch the whole chain: detection → coordinates
published → angles computed → nozzle aimed → pump runs → cooldown. The live
view's *Actuator board* panel shows the outcome of the last dispatch, and
says **why** if it was refused.

### 8.7 If the nozzle aims the wrong way

In order, because the causes are easy to confuse:

1. `GET /selftest` on the main board — is the solver itself right?
2. Is the yaw convention right? A **positive** yaw turns the camera's +Y
   direction toward **−X** (right-hand rule about +Z). The nozzle's pan is
   the other way round: pan 0 aims at +Y, positive pan aims at +X.
3. Is the pitch sign right? `cam_pitch` is positive when the camera looks
   **down**.
4. Is `cam_height` the height above the **pen floor**, not above the
   nozzle?
5. Is `focal_px` right? A quick check: `focal_px = frame_width / (2 × tan(hfov/2))`.

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| No stream | Verify camera GPIO pins in `components/camera/camera_service.c`, check ESP32 is flashed |
| Dashboard "Device unreachable" | Wrong `esp32_ip`/`esp32_port`, or ESP32 + PC on different LAN |
| Ping/`/status` time out but ESP is online | **Router wireless/client (L2) isolation** blocks station↔station. Fix: connect the PC to the router via **Ethernet**, or disable "AP/client isolation" in the router admin at `http://192.168.1.1` (Wireless → Advanced/Professional → Isolate). The ESP can still POST to the server; only PC→ESP pull is blocked |
| Need to change Wi-Fi/URLs | Open `http://<esp-ip>/setup` (no reflash), save, board reboots into the new network |
| `cam_hal: FB-OVF` in the serial log | **Normal — do not chase it.** The camera overruns because the detect loop consumes a frame every ~3.4 s. Measured at 8 occurrences in 70 s with the verifier perfectly healthy. See `docs/wiring-guide.md` |
| Verifier down and the board's detection goes sluggish | The verify poll runs inline in the detect loop, so a dead server blocks it. The firmware now fails a known-dead server in 2 s instead of 5 s; the underlying fix (moving the poll to its own task) is still open — see *Open issues* in `docs/session-notes.md` |
| ESP32 never appears on the phone hotspot | Hotspot is on 5 GHz — the ESP32-S3 is 2.4 GHz only. Also check the hotspot is not WPA3-only (`wifi_service.c` hardcodes `WIFI_AUTH_WPA2_PSK`) |
| Laptop and ESP32 both on the hotspot, but `/status` from the laptop times out | Windows Firewall on the **Public** profile blocking port 80 — see §7.1 step 3 |
| Dashboard loads locally but the ESP32 logs "Dashboard log failed" | Laptop's hotspot IP in `dash_url` is wrong, or Apache is not listening on the hotspot interface (§7.1 steps 3-4) |
| Stream/status blank on a phone browser | Expected — the dashboard talks to the device directly, and a phone cannot reach its own hotspot clients. View from the laptop (§7.4) |
| Verification empty | Python verify server not running; check `verify_url` in dashboard settings |
| CI3 `system folder path` error | Drop the CodeIgniter 3 `system/` dir into `dashboard/` |
| Database "table not found" | Run `dashboard/database.sql` import |
| Sprays nonstop | Threshold too low; raise it on the device |
| Main board gets no coordinates | The two boards are on different networks, or `target_url` is wrong. Check `GET /status` on the camera board: `target.published` counts what it sent and `target.failures` counts what did not arrive |
| Camera board sprays AND the main board sprays | The camera board's own pump is still on. `POST /config -d 'spray_enabled=0'` on the camera board (see §8.5) |
| Nozzle aims the wrong way | Work through §8.7 in order — `GET /selftest` first |
| Pump refused with `low_water` | Working as intended. Check `water_min` and the sensor's raw value in `GET /status` |
| Pump runs but nothing comes out | The pump's supply is separate and its own; check the hose and the relay's `NO` contact |
| Does not build | Install ESP-IDF v5.x, run `idf.py set-target esp32s3` |
