# Session notes

Running log of the current working setup and the traps that cost the most
time. Read this before debugging "it doesn't detect".

## Verified working — 2026-10-04 22:20–22:22

End-to-end detection through the repaired pipeline, in the `detections` table:

| id | event_time | confidence | yolo_conf | triggered | verified | notes |
|----|-----------|-----------|-----------|-----------|----------|-------|
| 191 | 22:20:09 | 0.2800 | 0.3434 | 1 | 1 | yolo server spray |
| 192 | 22:21:08 | 0.2266 | NULL | 1 | 0 | auto spray |
| 193 | 22:22:21 | 0.2800 | 0.3363 | 1 | 1 | yolo server spray |

`confidence` is exactly `0.2800` on server-gated rows because it is written as
the trigger threshold, not the model's score. The real number is in
`yolo_conf`. Rows with a NULL `yolo_conf` are on-device-only FOMO hits where
the verifier leg was down.

## Current addresses

| Thing | Value | Notes |
|-------|-------|-------|
| ESP32-S3 | `192.168.1.39` | **static**, pinned via `/config` |
| Laptop (XAMPP + verifier) | `192.168.1.2` | **DHCP — expect this to change** |
| Wi-Fi network | `Meliora 2` | Windows profile is **Public** |
| Verifier | port `8000` | `server/yolo_verify` |
| Dashboard | port `80` | `/feces-detector-ai/dashboard/` |

## The IP-drift trap (this is the big one)

The laptop gets its address by DHCP and it has already moved three times:
`docs` said `.3`, the board was provisioned with `.4`, it is now `.2`. Every
time it moves, the board keeps POSTing to a dead address and
`verifyOk:false` / `verifyFailures` climbs with no other symptom.

**Check `ipconfig` before trusting `/status`.** If the laptop's address has
changed, re-point the board:

```bash
curl -X POST http://192.168.1.39/config \
  --data-urlencode "verify_url=http://192.168.1.2:8000/verify" \
  --data-urlencode "dash_url=http://192.168.1.2/feces-detector-ai/dashboard/api/insert" \
  --data-urlencode "dash_verify_url=http://192.168.1.2/feces-detector-ai/dashboard/api/verify"
```

This **reboots the board** (~10 s). Only the three URLs need sending: the
handler seeds from NVS at `http_server_service.c:331`, so thresholds, static
addressing and relay polarity survive untouched. `GET /config` shows the
current values but deliberately never echoes `wifi_ssid` / `wifi_pass`.

Permanent fix, not yet done: reserve `192.168.1.2` for this laptop at
`http://192.168.1.1`.

## Power-cycling the ESP is safe

All runtime config lives in NVS (flash partition `0x9000`, `partitions.csv:2`),
written by `app_config_save()` in `app_config.c`. Power-cycling loses nothing —
the board comes back on `192.168.1.39` still pointing at `192.168.1.2`. Only
`idf.py erase_flash` wipes it, and then the SSID/password must be re-entered
over USB per `docs/setup-guide.md` §255-299.

## Laptop-side services (do not survive a reboot)

**Verifier** — start it first, every session:

```bash
cd server/yolo_verify
uvicorn app:app --host 0.0.0.0 --port 8000
```

Then confirm it is warm before blaming the board:

```bash
curl -s http://localhost:8000/health
```

Healthy means `"model_loaded": true` and `classes: {"0":"feces","1":"pig"}`.

**Firewall** — the Wi-Fi profile is Public, which blocks the board's inbound
connections. A rule named `YOLO Verify 8000` (inbound, TCP 8000, remote
`192.168.1.0/24`, interface `Wi-Fi`, profile Any) has been added and persists
in Windows. Re-create it if the machine is rebuilt:

```powershell
New-NetFirewallRule -DisplayName "YOLO Verify 8000" -Direction Inbound `
  -Protocol TCP -LocalPort 8000 -RemoteAddress 192.168.1.0/24 `
  -InterfaceAlias "Wi-Fi" -Action Allow -Profile Any
```

Needs an elevated shell.

## Confirming the whole path

```bash
curl -s http://192.168.1.39/status
```

Good: `wifiConnected:true`, `wifiRssi` around -60, `verifyOk:true`,
`verifyFailures:0`, `vrf.valid:true`. Bad: `verifyFailures` climbing means the
outbound leg is broken — check the verifier is up, the firewall rule exists,
and the laptop's IP is still `192.168.1.2`.

## Open issues

**The camera was aimed at a wall for several hours.** No bug in the pipeline —
`lastConfidence:0.0000` and `detections:0` were the correct answer to a photo
of a blank surface. The model reports `pig 0.408` / `feces 0.078` on an indoor
desk scene; those are texture false positives, not detections. Always pull a
frame and *look at it* before believing the detector is broken:

```bash
curl -s --max-time 12 -o s.bin http://192.168.1.39/stream
```

**Sensor delivers 640x480 despite `FRAMESIZE_HD`.** `camera_service.c:45`
requests 1280x720 but frames arrive VGA. That matters because
`prv_decode_jpeg` decodes at `JPEG_IMAGE_SCALE_1_8` → **80x60**, not the
160x90 its comment assumes, and `prv_downscale_rgb` then upscales 80x60 →
160x160. FOMO was chosen partly because the 10th-percentile feces blob is
~8.8 px at 160x160; upscaling from 80x60 smears that below one heat-map cell,
so small or edge-case feces stay invisible on-device. The server verifier is
unaffected — it runs on the full frame.

**Intermittent `ESP_ERR_HTTP_CONNECT`.** Serial shows
`esp-tls: select() timeout` → `HTTP_CLIENT: Connection failed, sock < 0` on
`/verify` while the server simultaneously logs `POST /verify 200 OK`. A TCP
*connect* failure, not HTTP or model. RSSI drifts -62…-77 dBm (the firmware's
own WEAK threshold is -75) and ping ranged 17 ms→762 ms, so connects get
starved. Expect missed verifier samples and `yolo_conf=NULL` rows.

**`FB-OVF` repeats in the serial log** — camera frame-buffer overflow. A
starved capture can also cost FOMO box quality.

**Model is undertrained.** `training/runs/detect/results.csv` stops at **epoch
6**; `evaluation_summary.txt` reads `mAP50 44.2%`, and the **feces class alone
is mAP50 15.1%** against a stated 80% objective. A ~0.28-0.34 hit is about the
ceiling of that checkpoint. Resuming training is the only route to the
objective. For reference the weights do behave sensibly on the val split —
feces scores of 0.68, 0.62, 0.36, 0.25 were observed, with pig dominating
most frames.

**Docs contain stale figures** — `setup-guide.md` still says the Wi-Fi service
"reconnects with no backoff" (it now backs off 1s→16s) and that the verifier
uses a 30 s backoff (now 4 s).