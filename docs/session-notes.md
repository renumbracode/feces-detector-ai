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

## Verified working — 2026-10-15 (GlobeAtHome_92CF6)

Board on `192.168.254.50`, static, RSSI **−31 dBm** on ch 5. `GET /status`
returned `vrf.valid: true`, `verifyOk: true`, `verifyFailures: 0` — so the
ESP32 → verifier leg on `192.168.254.107:8000` was working on the first try.
`GET /stream` returned `multipart/x-mixed-replace`, and the dashboard served
200. Ping averaged 59 ms with 1 of 3 probes lost.

## Current addresses

| Thing | Value | Notes |
|-------|-------|-------|
| ESP32-S3 | `192.168.254.50` | **static**, in NVS (see *Network migration* below) |
| Laptop (XAMPP + verifier) | `192.168.254.107` | **DHCP — reserved at the router, but expect this to change** |
| Wi-Fi network | `GlobeAtHome_92CF6` | Windows profile is **Public** |
| Verifier | port `8000` | `server/yolo_verify` |
| Dashboard | port `80` | `/feces-detector-ai/dashboard/` |

> `192.168.254.0/24`, gateway `192.168.254.254`. Note this is **not** the usual
> `192.168.1.0/24` a home router hands out — GlobeAtHome uses `.254`. Anything
> hardcoding `192.168.1.x` is stale by assumption, not by accident.

## Network migration — 2026-10-15 (Meliora → GlobeAtHome_92CF6)

Moved to a **GlobeAtHome** router. Two things differ from every previous network
and both break the §7.5 runbook if you assume otherwise:

- **The LAN is `192.168.254.0/24`, not `192.168.1.0/24`.** Gateway is
  `192.168.254.254`. The laptop came up on `.107`, so all three of the board's
  URLs changed.
- **`Meliora` was out of range entirely** — `netsh wlan show networks` listed
  only the new SSID, so the board was unreachable *and* its stored SSID could
  not re-associate. `POST /config` had no path in, so this was the USB NVS
  rewrite (`setup-guide.md` §7.5, full-config variant) again. Back up first:

  ```bash
  python -m esptool --chip esp32s3 --port COM4 read_flash 0x9000 nvs_backup.bin 0x6000
  ```

No firmware change was needed. The AP is **2.4 GHz / WPA2-Personal / ch 5**,
which satisfies the hardcoded `WIFI_AUTH_WPA2_PSK` in `wifi_service.c`.

### The firewall warning in §7.1 does not always apply

§7.1 step 3 says the Public profile blocks inbound80/8000 and tells you to fix
it in `wf.msc`. On this machine that was **already handled**: allow rules for
`Apache HTTP Server` and `python.exe` exist on the **Public** profile. Check
before editing anything:

```bash
powershell -Command "Get-NetFirewallRule -Enabled True -Direction Inbound -Action Allow \
  | Where-Object { \$_.DisplayName -match 'Apache|Python' } | Select DisplayName,Profile"
```

Switching the profile to Private needs elevation and is blocked by policy
(`Network List Manager Policies`) on this box, so prefer confirming the rules
exist over trying to change the category.

### Reading the tuning values back before you overwrite them

A NVS rewrite resets every key not in the CSV. Rather than guessing what the
board was running, dump the partition first:

```bash
python components/nvs_flash/nvs_partition_tool/nvs_tool.py -d minimal nvs_backup.bin
```

`threshold` and `srv_trig_conf` print as empty — they are blobs, and `nvs_tool`
shows no value for them. Read the 4 bytes straight out of the image instead;
an entry is `[ns:1][type:1][span:1][chunk:1][crc:4][key:16][data:8]`, and the
blob payload sits in the next `span-1` entry slots:

```bash
python -c "
d=open('nvs_backup.bin','rb').read()
for off in range(0,len(d)-32,4):
    if d[off+8:off+24].rstrip(b'\x00') in (b'threshold',b'srv_trig_conf'):
        print(d[off+8:off+24].rstrip(b'\x00').decode(), d[off+32:off+36].hex())"
# threshold 9a99193f      srv_trig_conf 295c8f3e   -> 0.60 and 0.28
```

Those values are then re-emitted as `hex2bin` rows. Here they came back at the
compiled defaults, but they are still worth reading rather than assuming.

### Router-side checks still owed

`192.168.254.50` was confirmed free before flashing (no ARP reply), but the
**DHCP pool range was not checked** — if the pool includes `.50`, a future lease
can collide with the board's static address. Also unverified: **AP/client
isolation**, which would leave detection working while the dashboard's stream
and status tiles go blank (`session-notes.md`, troubleshooting table).

## Network migration — 2026-10-13 (ALTHEA 11 → Meliora)

Back to `Meliora`, which is `192.168.1.0/24` — the same subnet as `Meliora 2`
was before the ALTHEA detour. Provisioned again by USB NVS rewrite
(`setup-guide.md` §7.5, full-config variant) because the board was off-network
and `POST /config` had no path to it. Restored to `192.168.1.39` static, so the
dashboard's `esp32_ip` matches the historical default again.

**Meliora broadcasts on both bands, but Windows will only show you the 2.4 GHz
BSSID if that radio is actually enabled.** On arrival the scan listed exactly
one BSSID for `Meliora` and it was 5 GHz channel 132 — which the ESP32-S3
cannot see at all, since it has no 5 GHz radio. This is the failure to check
*before* flashing anything: the board then associates with nothing and the boot
log just reads `AP unreachable after 30 s`, which is indistinguishable from a
password typo.

```bash
netsh wlan show networks mode=bssid | tr -d '\r' | grep -A 3 "^SSID.*Meliora"
# want two Band lines: 5 GHz and 2.4 GHz
```

Enable the 2.4 GHz radio at `http://192.168.1.1` first. After it came up it
advertised 2.4 GHz channel 11 at 100%.

Result, versus ALTHEA 11: **`wifiRssi` -65 dBm at association, settling to
-52**, against -67…-73 with ~950 ms pings on ALTHEA 11. Link quality is
markedly better here, so expect fewer missed verifier polls.

`verify_url` was still `http://192.168.1.4:8000/verify` in the dashboard — a
fourth stale address nobody remembered — and `esp32_ip` still `192.168.100.39`.
Both fixed; `stream_url` was already clear.

**Reserved `192.168.1.6` at the router** for this laptop (MAC
`50:2e:91:5e:94:3c`). The laptop address has moved five times now, and each
move silently breaks the board's outbound POSTs.

### The firewall rule no longer needs re-scoping

The `YOLO Verify 8000` rule used to be scoped to a single subnet, which meant
it had to be re-scoped by hand every time this laptop moved networks — it bit
on the 2026-10-12 and 2026-10-13 moves, and on 2026-10-27 it was still scoped
to `192.168.100.0/24` (the ALTHEA 11 subnet) while the board sat on
`192.168.254.50`.

**Fixed 2026-10-27:** the rule is now `RemoteAddress=Any`, so it matches on
every network this laptop joins and can never go stale again:

```powershell
Set-NetFirewallRule -DisplayName "YOLO Verify 8000" -Action Allow
Get-NetFirewallRule -DisplayName "YOLO Verify 8000" |
  Get-NetFirewallAddressFilter |
  Set-NetFirewallAddressFilter -RemoteAddress Any
```

It still only opens **TCP 8000 inbound**, and the verifier only binds
`:8000` while it is running.

Two things worth knowing:

- This adds **no new exposure**. `C:\xampp\apache\bin\httpd.exe` and the
  Python 3.12 interpreter already carry Public- and Private-profile inbound
  allows with `PORT=Any` and `RemoteAddress=Any`, so the verifier leg worked
  regardless of what this rule said. The rule is belt-and-braces, not the only
  thing standing between the board and the verifier.
- `Set-NetFirewallRule` and friends need an **elevated** shell. From a
  non-elevated one they fail with `Access is denied. (System Error 5)` and
  silently change nothing — the trailing `Get-` still prints the old scope,
  which reads like a partial success. Check the result, not the exit code.

## Live view blank after a network change — check the DB, not the board

On the ALTHEA 11 migration the live view stayed blank even though the board was
fine. Cause: the dashboard's `esp32_ip` setting still held the old `192.168.1.39`.

```sql
UPDATE feces_detector.settings SET value='192.168.100.39' WHERE `key`='esp32_ip';
UPDATE feces_detector.settings SET value=''               WHERE `key`='stream_url';
```

**Clear `stream_url`, always.** `MY_Controller.php:20-26` returns the DB
`stream_url` when it is non-empty and only falls back to
`http://{esp32_ip}:{esp_port}/stream` when it is empty — so a stale
`stream_url` silently overrides `esp32_ip` and points the `<img>` at a dead
address. This is the same class of bug as the IP-drift trap below: the device
is healthy and the dashboard is lying.

Confirm the fix by checking the rendered HTML rather than trusting the database:

```bash
curl -s http://localhost/feces-detector-ai/dashboard/live | grep -o 'src="[^"]*"'
# want: src="http://192.168.100.39/stream"
```

Note the dashboard's own `verify_url` setting is `http://192.168.1.4:8000/verify`
and is **still stale**. It does not break the live view — it is consumed by PHP
on this laptop, so it should be `http://localhost:8000/verify`, not the board's
address.

## Network migration — 2026-10-12 (Meliora 2 → ALTHEA 11)

Moved from `Meliora 2` (`192.168.1.0/24`) to `ALTHEA 11` (`192.168.100.0/24`).
The board was provisioned on the old network with a **static** `192.168.1.39`,
so after the switch it was unreachable: wrong subnet *and* the NVS SSID no
longer existed. Recovery was the USB NVS rewrite of `setup-guide.md` §7.5 —
`/config` was unusable precisely because it needs the network that broke.

Rewritten in one shot, so no round-2 `POST /config` was needed:
`wifi_ssid`/`wifi_pass`, all three URLs, the full `static_ip`/`static_mask`/
`static_gw` trio, and the tuning values. Worth knowing for next time:

- **`threshold` and `server_trigger_conf` must be `hex2bin` 4-byte LE floats**,
  not `u32`. They are stored with `nvs_set_blob` of a raw `float`
  (`app_config.c:167,171`), so `0.60` is `9a99193f` and `0.28` is `295c8f3e`.
  Writing them as `u32` silently leaves the compiled defaults standing — no
  error, the value is just wrong.
- **All three `static_*` keys or none.** A partial set is discarded back to DHCP
  (`app_config.c:83`), so the board comes up at an address nobody knows.
- Under static addressing `GOT_IP` never fires, so the boot log shows
  `app_cfg: Static IP configured: ...` + `wifi_svc: WiFi connected`.
  `AP unreachable after 30 s` means SSID/password/WPA mode is wrong — the board
  then keeps detecting and spraying offline, so it looks alive but is not.
- `wifiRssi` sat at **-67 to -73 dBm** on ALTHEA 11, worse than the -62 to -77
  range on Meliora 2, and ping averaged ~950 ms with spikes to 1.8 s. That is
  the starvation already described below; expect missed verifier polls here.

## The IP-drift trap (this is the big one)

The laptop gets its address by DHCP and it has already moved four times:
`docs` said `.3`, the board was provisioned with `.4`, then `.2`, and it is now
`192.168.100.227` on a different subnet entirely. Every time it moves, the
board keeps POSTing to a dead address and `verifyOk:false` / `verifyFailures`
climbs with no other symptom.

**Check `ipconfig` before trusting `/status`.** If the laptop's address has
changed, re-point the board:

```bash
curl -X POST http://192.168.100.39/config \
  --data-urlencode "verify_url=http://192.168.100.227:8000/verify" \
  --data-urlencode "dash_url=http://192.168.100.227/feces-detector-ai/dashboard/api/insert" \
  --data-urlencode "dash_verify_url=http://192.168.100.227/feces-detector-ai/dashboard/api/verify"
```

This **reboots the board** (~10 s). Only the three URLs need sending: the
handler seeds from NVS at `http_server_service.c:331`, so thresholds, static
addressing and relay polarity survive untouched. `GET /config` shows the
current values but deliberately never echoes `wifi_ssid` / `wifi_pass`.

Permanent fix, not yet done: reserve `192.168.100.227` for this laptop at
`http://192.168.100.1`.

## Power-cycling the ESP is safe

All runtime config lives in NVS (flash partition `0x9000`, size `0x6000`),
written by `app_config_save()` in `app_config.c`. Power-cycling loses nothing —
the board comes back on `192.168.100.39` still pointing at `192.168.100.227`.
Only `idf.py erase_flash` wipes it, and then the SSID/password must be
re-entered over USB per `docs/setup-guide.md` §7.5.

> The `nvs` address comes from ESP-IDF's built-in single-app partition table,
> **not** from the repo's `partitions.csv` — `CONFIG_PARTITION_TABLE_CUSTOM=n`
> in `sdkconfig.defaults` makes the build ignore that csv entirely. The live
> layout is `nvs 0x9000/24K`, `phy_init 0xf000/4K`, `factory 0x10000/1M`. There
> is **no `storage` fat partition** on the board and nothing uses one.
>
> **The app partition is 99.2 % full** — 8,352 bytes free of 1,048,576 as of
> the 2026-10-27 build. A firmware change that adds more than a few hundred
> bytes will not fit, and the build only warns ("nearly full (1 % free space
> left)") without failing. Check the size after every build:
>
> ```bash
> python -c "import os;n=os.path.getsize('build/esp32s3_fomo.bin');print(n,1048576-n,'free')"
> ```

**Careful:** rewriting the NVS partition over USB *does* reset every key not in
the CSV — threshold, cooldown, spray timing, relay polarity and the URLs all
fall back to the compiled defaults. Writing the whole set by hand avoids the
round-2 `POST /config`, but it also means the values are whatever you typed, not
whatever the board had been running. See *Network migration* above.

## Reflashing the app does NOT need the NVS runbook

The §7.5 USB NVS rewrite is only for a board that has **left the network**.
An ordinary firmware update does not touch NVS at all, so the SSID, password,
static IP and the three URLs all survive.

`build/flash_args` contains only three entries — bootloader `0x0`, partition
table `0x8000`, app `0x10000`. NVS lives at `0x9000` and is not in that set, so
even a full `idf.py flash` would leave it alone.

The minimal-risk form is to write **only the app partition**, leaving the
bootloader, the partition table and NVS completely untouched:

```bash
PY=/c/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe
$PY -m esptool --chip esp32s3 --port COM4 --baud 460800 \
    --before default_reset --after hard_reset \
    write_flash 0x10000 build/esp32s3_fomo.bin
```

Verified end to end on 2026-10-27: the board came back on `192.168.254.50`
with `verify_url`, `dash_url`, `dash_verify_url`, `threshold 0.6`,
`server_trigger_conf 0.28`, `cooldown_ms 30000`, `spray_ms 180000` and
`relay_active_low true` all intact — confirmed by `GET /config`, which echoes
every key except the Wi-Fi credentials.

Back up first anyway, to a directory **outside the repo** and outside the
Apache web root (the NVS image contains the Wi-Fi password):

```bash
mkdir -p ~/firmware_backup && cd ~/firmware_backup
$PY -m esptool --chip esp32s3 --port COM4 read_flash 0x9000 0x6000 nvs_backup.bin
$PY -m esptool --chip esp32s3 --port COM4 read_flash 0x10000 0x100000 app_backup.bin
```

The board enumerates on COM4 without needing BOOT/RESET — `Failed to connect`
is a symptom of the USB cable not being a data cable, not of a missing
handshake. Opening the port with pyserial resets the board, because the
USB-Serial/JTAG bridge drives EN from DTR/RTS; that is harmless but it means
a "capture the boot log" tap is also a reboot.

## Two boards: the camera board and the actuator board — 2026-10-28

The system is now split across **two ESP32 boards on the same Wi-Fi**:

```
LAPTOP A (this repo)                     LAPTOP B
─────────────────────                    ────────
ESP32-S3 + OV3660 camera                 ESP32 ("main")
  ├ FOMO detection (multi-instance)        ├ 2 servos (pan/tilt nozzle)
  ├ YOLOv8 verify poll                      ├ relay → water pump
  ├ POST /target ────────────────────────► ├ water-level sensor
  └ its OWN local spray path (unchanged)    └ HTTP :8080 /target /status /servo /pump
Python YOLOv8 verifier (localhost:8000)
CodeIgniter dashboard (localhost)
```

The camera board detects and **publishes coordinates**; the main board
**converts them to servo angles and decides whether to spray**. The wire
contract is `docs/target-protocol.md`.

**The camera board's wiring is deliberately UNCHANGED.** Its relay, LED,
buzzer, all 14 camera pins and its local spray path are exactly as they
were, because that board may be reused for something else. The publisher is
strictly additive.

### The consequence that must not be missed

Because the camera board keeps its own pump, **both boards would spray for
one detection**. The camera board's existing kill switch is the fix:

```bash
curl -X POST http://192.168.254.50/config -d 'spray_enabled=0'
```

Do this **before** the main board goes live. It is step 8.5 of the setup
guide and it is not optional.

### Why the split was forced by the hardware, not chosen

The camera board has **no free pins**. The OV3660 takes 15, 4–13 and 16–18;
the relay takes 14; the LED takes 1; the buzzer takes 21. `docs/wiring-guide.md`
lists the only remaining safe pin as GPIO2, which is best avoided because on
many devkits it is an addressable RGB LED. Two servos plus a water sensor
**cannot** fit on the camera board — so the split was the only option, which
is also exactly what the requirements asked for.

### The app partition ran out of room, and that fixed a long-standing wart

The camera board's app partition was **99.3 % full** (7,840 bytes free of
1,048,576) and the publisher would not fit. The fix was to enable the repo's
own `partitions.csv`:

```
CONFIG_PARTITION_TABLE_CUSTOM=y
```

which had been `n` all along, so **the build had been ignoring
`partitions.csv` entirely** and using ESP-IDF's built-in single-app table.
The app partition is now 0x1E0000 (1.875 MB) with ~919 KB free, and the fat
`storage` partition the csv declared actually exists. NVS is at 0x9000/24K in
**both** layouts, so the Wi-Fi credentials, the static address and the URLs
survived — verified by `GET /config` after the reflash.

### The geometry solver had a sign error, and the test caught it

The camera's local "down" vector tilts **backward** as the camera pitches
down — the image's vertical axis swings, not the view direction. With the
wrong sign, a pixel above the centre solves to a point **closer** to the
camera instead of farther away. The error is invisible until a servo sweeps
the wrong way.

It was caught by writing an **independent implementation of the same model**
and comparing the outputs, before anything was flashed. The solver is a pure
function precisely so it can be checked; `GET /selftest` on the main board
runs the same assertions on the real target.

## Detection was broken by the verifier, not by the model — 2026-10-29

**The symptom:** feces pointed at the camera, the board reported
`detected: false`, `feces_count: 0`, and nothing sprayed. The multi-box
code was fine the whole time; **two lines in `server/yolo_verify/app.py`
were silently discarding every detection.**

### 1. `MAX_IMAGE_SIZE` was 416, and that halved the confidence

The camera uploads a **640x480** JPEG. The server resized it to 416x312,
and Ultralytics then **letterboxed it back up to 640** for inference — so
the picture reached the network as a blurred 416 -> 640 round trip.

The comment said "to keep inference fast". **It bought no speed at all:**
measured 139 ms/frame at 640 vs 142 ms at 416 (the cost is the convolution
stack, not the input pixels). It only destroyed signal. Measured on the
frames the board actually uploaded:

| | 416 (old) | 640 (fixed) |
|---|---|---|
| mean best-feces score | 0.1877 | **0.2342** |
| frames firing at 0.15 | 6/8 | **8/8** |

640 is also the model's native `imgsz` (see `training/runs/detect/args.yaml`).

### 2. `YOLO_CONF` was 0.25, above what this model produces

The feces class is weak on its own — **mAP50 15.1%** (`evaluation_summary.txt`)
against a 73.3% for pig. A plainly visible blob scores **0.15-0.45**, not
0.8+. So a 0.25 floor answered "no feces" for frames where the blob was
right there in the picture. It is now **0.15**.

`YOLO_CONF` is the floor for *reporting* a detection. **It is not the spray
gate** — that is the board's own `server_trigger_conf` (now 0.20). The
false-positive cost is real and is why the actuator board's `disabled`
switch and the camera board's local spray switch matter: on the 22 pig-only
dataset images, a 0.25 floor fires on 7/22 and 0.15 fires on 18/22.

### The lesson

**`boxes_norm` being empty is not evidence the model found nothing.** The
server had a floor, and the model was below it. To tell "the model saw
nothing" from "the server dropped what the model saw", score the frame
offline at a low threshold and compare — that is what found this. The
device's own log is the other half of the story: `verify poll: detected=1
conf=0.260 feces=2` in the serial log while `/status` showed zeros was the
tell that the *reporting* layer, not the detector, was at fault.

Note that `/status` on the camera board reports `vrf.*` from the last
*accepted* poll, and opening the serial port **resets the board** (the
USB-Serial/JTAG bridge drives EN from DTR/RTS). Reading the log
therefore reboots it, and the first `/status` afterwards shows zeros until
the next poll lands.

### The actuator board moved address

It is now **192.168.254.111**, not `.112` (DHCP). The camera board's
`target_url` and the dashboard's `settings.target_url` must both be
`http://192.168.254.111:8080/target`.

**Two bugs in that URL, both fixed.** The port was in the *path*
(`/8080/target` instead of `:8080`), and the firmware **percent-encodes
the URL the dashboard sends and never decodes it**
(`http_server_service.c:333` uses `httpd_query_key_value`, which does not
decode), so the stored value became `http%3A%2F%2F...` and the HTTP client
rejected it: `Error parse url` -> `cannot create client for ...`. The
dashboard sends `rawurlencode()`d URLs (`Control.php:55`) and `POST /config`
decodes them (`prv_get_decoded`) but `GET /target` does not. The stored
value now reads back unencoded.

### 2026-10-31: that decoding fix was NOT in the code, and the link was dead

The note above says both URL bugs were fixed. **They were not** — the
`GET /target` handler still used the raw `httpd_query_key_value`, so every
push from the dashboard stored `http%3A%2F%2F...` again. The board was
publishing to a string the HTTP client could not parse:

```
published: 717   failures: 716
url: http%3A%2F%2F192.168.254.111%3A8080%2Ftarget
```

Two separate faults, and they are easy to confuse:

1. **The URL was percent-encoded** (never decoded) *and* carried a stale
   port. Fixed in the firmware: `GET /target` now uses the same
   `prv_get_decoded` helper `POST /config` already used. The stored value
   reads back unencoded, verified on the board.
   A board whose NVS still holds the broken value can be repaired without
   reflashing: `POST /config` decodes, so
   `curl -X POST http://<camera>/config -d 'target_url=http%3A%2F%2F<host>%3A<port>%2Ftarget'`
   stores it correctly (that POST reboots the board).
2. **The board at .111 is not this repo's `esp32_main` firmware.** It
   serves on **port 80**, has no `/config`, `/setup` or `/selftest`, and
   answers `POST /target` with `{"ok":true,"ack":true,"cleaning":false}`
   rather than `{"ok":true,"accepted":true,"pan":...}`. Its `/status` has a
   different schema (`system`, `pump`, `cleaning`, `db`, `logs`) and it
   polls the camera itself, so it already receives targets that way. Its
   strings (`"Target received from camera"`, `"ack"`) appear nowhere in this
   repository, so that firmware lives elsewhere.
   The publisher now accepts `ack` as accepted too, and reports whether the
   board sent angles at all (`lastAngles`) — otherwise the dashboard printed
   "pan 0.0°" for a spray that had really happened.

So the URL in `settings.target_url` must match the port the deployed board
actually serves (**80** for the board that is running now, **8080** for this
repo's `esp32_main`). It is a setting; nothing else depends on it.

## Laptop-side services (do not survive a reboot)

**Verifier** — start it first, every session:

```bash
scripts/start_verifier.sh          # Git Bash / any POSIX shell
scripts\start_verifier.cmd          # Windows cmd or PowerShell
scripts/start_verifier.sh stop      # or: scripts\start_verifier.cmd stop
```

Both start `uvicorn app:app --host 0.0.0.0 --port 8000` from
`server/yolo_verify` with stdout/stderr redirected to `logs/yolo_verify.log`
and `logs/yolo_verify.err.log`, then wait for `/health` to report
`model_loaded: true` before returning. Neither starts a second copy if the port
is already listening, and `stop` kills whatever owns the listening socket.

`.cmd` keeps the actual launch in a helper, `scripts\_verifier_run.cmd`, and
starts it with `start` — `start` creates the child with
`bInheritHandles=FALSE`, so the server cannot inherit the caller's handles.
`Start-Process` does *not* do that: it leaks a duplicate of the caller's pipe
into the server, so a tool or agent that captures output never sees EOF and
appears to hang until the server exits. Run the `.cmd` from a real console;
under Git Bash prefer the `.sh`, which detaches cleanly.

**Never launch the verifier with a bare `uvicorn app:app` from a tool or agent
shell.** The bare command inherits stdout/stderr from the calling shell, and
that parent is usually a session capture *pipe* which stops being drained once
the call ends. The pipe fills, python blocks inside `write()`, and uvicorn —
which binds the socket only *after* the app's startup event — never reaches
`create_server()`. What you get is a process that looks perfectly healthy in
Task Manager (1.5 GB RSS, 343 threads, torch fully loaded, model weights
resident) but has **no listener on :8000 at all**, and the board reports
`verifyOkAgeMs = UINT64_MAX` forever while `verifyFailures` climbs.

This happened on 2026-10-27 and cost a session: the process had been up for
~6 h with `netstat` showing nothing on :8000 and `curl` getting connection
refused. The giveaway is that the process is **idle** — 0.06 s of CPU over a
6 s window, 341 of 343 threads in `Wait / UserRequest` — while still holding
the model in memory. A fresh `import app` on the same box takes 11.7 s, so
the code and weights were never the problem.

Redirecting stdout/stderr to files removes the pipe, and with it the failure
mode. `scripts/start_verifier.sh` and `.cmd` exist precisely to make that the
default path.

### The launcher also has to survive a Ctrl+C (2026-10-28)

The first version of `start_verifier.cmd` used `start /min cmd /c`, which
creates a **console**. A console can receive a Ctrl+C — and closing that
window, or any Ctrl+C reaching the console group, killed the server with a
`^C` in `logs/yolo_verify.err.log`. It happened **twice in one session**.

The launcher now uses **`CreateNoWindow`** (via PowerShell's
`ProcessStartInfo`), so the server has **no console at all** and nothing can
Ctrl+C it. The only way to stop it is `start_verifier.cmd stop`. The POSIX
script uses `setsid` for the same reason (`nohup` alone only ignores
SIGHUP, so a Ctrl+C to the foreground process group still gets through).

Verified against both failure modes:

| Test | old launcher | new launcher |
|---|---|---|
| parent `cmd.exe` killed (window closed) | server died | **survived** |
| Ctrl+C to the shell's process group | server died | **survived** |
| `^C` in the error log | 1 | **0** |

File redirection to `logs/yolo_verify.log` and `logs/yolo_verify.err.log` is
kept exactly as it was — that is the anti-wedge property, and it is
independent of the console question.

### It now starts itself at logon

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_verifier_task.ps1
```

registers a per-user scheduled task (`FecesDetectorVerifier`) that runs the
launcher at logon. No admin needed. The launcher refuses to start a second
copy, so running it by hand while the task has it running is harmless.

`… -Status` shows the task and its last result; `… -Remove` unregisters it.

Then confirm it is warm before blaming the board:

```bash
curl -s http://localhost:8000/health
```

Healthy means `"model_loaded": true` and `classes: {"0":"feces","1":"pig"}`.

If the port is refusing connections but a python process is still around,
check `netstat -ano | findstr :8000` — no `LISTENING` row means it wedged as
above, and `scripts\start_verifier.cmd stop` plus a restart is the fix.

**Firewall** — the Wi-Fi profile is Public, which blocks the board's inbound
connections. A rule named `YOLO Verify 8000` exists in Windows. It is scoped
to **any remote address** since 2026-10-27, so it no longer needs re-scoping
when this laptop changes network — see *The firewall rule no longer needs
re-scoping* above for the command and the caveats.

## Confirming the whole path

```bash
curl -s http://192.168.1.39/status
```

**`verifyOk:true` alone proves nothing.** `s_verify_ok` is initialised to `true`
(`detection_service.c:51`) and only ever flipped by a failed poll, so on a board
that has never detected anything it reads `true` while the verifier is
completely unreachable. Check `verifyOkAgeMs` instead: it is `UINT64_MAX`
(`18446744073709551615`) until a poll is first *accepted*
(`detection_service.c:486`), so that value means "never verified" no matter what
`verifyOk` says.

Good: `wifiConnected:true`, `wifiRssi` around -60, `verifyOkAgeMs` a normal
number, `verifyFailures:0`, `vrf.valid:true`. Bad: `verifyFailures` climbing means the
outbound leg is broken — check the verifier is up, the firewall rule, and the
laptop's IP is still what the board points at.

### Reading the board's own verify log

Since 2026-10-27 the board logs **every** verify poll at INFO, including the
round-trip time, so a failure is diagnosable from the serial log alone:

```
I (10779) detect: verify poll: detected=0 conf=0.000 feces=0 box=0.00,0.00 0.00x0.00 (took 610 ms)
```

and on failure one of:

```
E/I detect: verify poll failed: <err> (sent N B, timeout 5000 ms, took NNNN ms)
I        detect: verify poll rejected: HTTP 0 (sent N B, took NNN ms)
```

**`HTTP 0` is the interesting one.** `esp_http_client_perform()` returns
`ESP_OK` even when the response never arrived in full, leaving the status code
at 0 — so the board counts the poll as failed while the *server* logs a clean
200. A real HTTP error always has a 3-digit status. Combined with the elapsed
time it separates the causes:

| symptom | meaning |
|---|---|
| `HTTP 0`, elapsed ≈ 5000 ms | the response was truncated or never arrived (TCP reset, or a server too slow to answer within the 5 s timeout) |
| `HTTP 0`, elapsed ≪ 5000 ms | the connection was closed immediately |
| `verify poll failed: <err>` | the client reported a transport error by name |
| `HTTP 4xx/5xx` | a genuine HTTP rejection |

These lines used to be `ESP_LOGD`, and there is no `esp_log_level_set()`
anywhere in this firmware, so at the default INFO level they were **compiled
out entirely** — which is why an intermittent failure in the 2026-10-27 session
could not be explained. The board's boot log now also carries a build stamp:

```
I (689) fomo_main: Boot: ESP32-S3 Feces Detector (FOMO + YOLOv8 verify) idf=v5.5.5 build=Oct 27 2026 23:52:27
```

so "which firmware is actually on the board" is answerable without assuming
the flashed binary matches the working tree.

**Note:** the poll cycle is ~2-4 s, so this logs a line every few seconds
whenever a monitor is attached. That is the intended trade — it is the only
place the board's view of the verify leg is visible.

## Open issues

**The verify poll runs inline in the detect loop, so an unreachable verifier
blocks detection.** When the laptop was on a different network the board's ARP
for it never resolved, so the *connect* alone burned the full 5 s timeout on
every attempt. With a 4 s backoff that is ~5 s of every 9 s spent blocked,
and the camera frame buffer overruns meanwhile (`cam_hal: FB-OVF`).

Partly mitigated on 2026-10-28: the poll timeout is now **adaptive** —
`VERIFY_TIMEOUT_MS` (5 s) while the verifier is healthy, and
`VERIFY_TIMEOUT_MS_FAILED` (2 s) once a poll has failed. The healthy path is
unchanged, so a slow-but-working server is never cut off.

**The real fix is to move the poll onto its own task**, so the detect loop
never blocks on HTTP at all. It was deliberately NOT done here, because the
poll is entangled with the dashboard row linkage: the current order is
`insert row -> poll verifier -> write yolo_conf back onto that row`, and
restructuring it changes when the server-gated spray fires and how the
history row is written. That is a bad thing to change while bringing up new
hardware. It is the first thing to do once the two-board system is working.

### `cam_hal: FB-OVF` is normal

Measured 2026-10-28: **8 occurrences in 70 s with the verifier perfectly
healthy** (poll round trips 474–897 ms, every poll succeeding). The camera
produces frames at ~25 fps while the detect loop consumes one every ~3.4 s
(720 ms of FOMO inference plus the poll), so the driver overruns and drops
frames. The delivered frames are good and detection works. **Do not chase
this** — it was previously mistaken for a fault.

**The camera was aimed at a wall for several hours.** No bug in the pipeline —
`lastConfidence:0.0000` and `detections:0` were the correct answer to a photo
of a blank surface. The model reports `pig 0.408` / `feces 0.078` on an indoor
desk scene; those are texture false positives, not detections. Always pull a
frame and *look at it* before believing the detector is broken:

```bash
curl -s --max-time 12 -o s.bin http://192.168.1.39/stream
```

Still true on 2026-10-12: `/stream` served a clean 640x480 frame of a blank
surface and `detections:0` was again correct. Note `s.bin` is a **multipart**
MJPEG body, not a JPEG — `PIL` cannot open it directly, so slice from the
`\xff\xd8\xff` marker to the last `\xff\xd9` before viewing it.

**MySQL was corrupt and the dashboard is down (2026-10-12) — now fixed, see
below for the real cause.** Every connection — `root` and any other user, over
TCP and named pipe — failed with
`ERROR 1130 Host 'localhost' is not allowed to connect`. This was **not** a
credentials problem and no password change would have fixed it. The
`mysql_error.log` line below is real but was a *misleading* symptom:

```
[ERROR] InnoDB: Database page corruption on disk or a failed file read of
tablespace mysql/transaction_registry page [page id: space=3, page number=0]
```

**FIXED 2026-10-12 — the 1130 was Aria corruption in the grant tables, not
InnoDB.** The `mysql_error.log` line above is real but it is a *second* problem,
and I wasted time chasing it first. Getting in with `--skip-grant-tables` and
running `CHECK TABLE` gave the actual answer:

```
check table mysql.global_priv;
| status | warning | Size of datafile is: 60314 | Expected: 16384
| status | error   | Page 0: Got error: 176 when reading datafile | Corrupt
```

Error 176 is a **bad Aria page checksum**. `global_priv` is the table that
holds every MySQL account in MariaDB 10.4 (`mysql.user` is only a view over
it), so once it cannot be read the server loads an empty account list and
answers 1130 to *everyone* — including a deliberately bogus username, which is
the tell that it is not a credentials problem. The 60314-vs-16384 size mismatch
is the giveaway: the data files were double their expected size.

`REPAIR TABLE` returns `status OK` but silently drops all rows (4 → 0), because
the data pages are gone. **`OK` from REPAIR does not mean the data survived.**
The repair also does not fix the index, and any later `FLUSH PRIVILEGES` re-loads
the grant table, re-enables enforcement *even under `--skip-grant-tables`*, and
locks you out again. That is the trap: `FLUSH PRIVILEGES` under
`--skip-grant-tables` is not a way to create users.

What actually works, and preserves every application database:

```bash
# 1. back up the datadir with mysqld STOPPED (copying a live one can tear)
# 2. mysql_install_db.exe --datadir=C:/some/scratch/dir   # clean grant tables
# 3. copy scratch/mysql/{global_priv,db}.{MAD,MAI,frm} over the corrupt ones
# 4. start mysqld normally -- root@localhost logs in, grants enforced
```

Do **not** run `mysql_install_db` against the live datadir: it wants to populate
the whole thing and will clobber the four application schemas. Build it in a
scratch directory and copy in only the grant files. Also move
`mysql/transaction_registry.ibd` aside on the way (it is the InnoDB corruption
from the log; the table only tracks in-flight XA transactions and InnoDB
recreates it).

Never `mv` the Aria files away as a first move. The `.frm` alone is not enough —
Aria will not rebuild a missing `.MAD`/`.MAI`, so `mysql.user` becomes
unreadable and you are worse off than when you started. Restore the datadir
from backup and rebuild instead. Full backup, SQL dumps and the scratch datadir
are in `%TEMP%/opencode/`.

Post-fix state: all 54 application tables intact, `detections` preserved at 70
rows / id 195, `campus_biometric` (27), `drr_dulag_db` (6), `phpmyadmin` (19)
untouched. Accounts recreated as `root`@`localhost`/`127.0.0.1`/`::1` plus
`pma`@`localhost`. **The original accounts were unrecoverable**, so if
`campus_biometric` or `drr_dulag_db` used its own DB user it must be re-created
by hand — check that code for hardcoded credentials. A harmless-looking
`ERROR 176` can still appear on `GRANT` statements; the write lands anyway, and
`CHECK TABLE` afterwards reads `OK`.

**Sensor resolution — this entry was WRONG until 2026-10-13, and it was read as
a hardware fault for weeks.** The old note here claimed "Sensor delivers 640x480
despite `FRAMESIZE_HD`", implying the driver was silently clamping and the
1/8 decode was yielding 80x60. That was never true. Parsing the SOF marker of a
live `/stream` capture shows the OV3660 was delivering a clean **1280x720** at
`FRAMESIZE_HD`, and `prv_decode_jpeg` was producing 160x90 exactly as its comment
said. There was no clamping.

What made it matter: a 1280x720 frame at `jpeg_quality 30` measured **25,753
bytes**, not the 10-15 KB that `camera_service.c` claimed in a comment. Against
`STREAM_BUDGET_BPS 61440` that capped the live view at ~2.4 fps by arithmetic,
before the link was even considered. The comment's 10-15 KB estimate was
sized for a frame the sensor never produced.

Fixed 2026-10-13 — see *Live view was capped at 2 fps by a frame-size estimate*
below. `CAM_FRAMESIZE` is now `FRAMESIZE_VGA` with a matching
`JPEG_IMAGE_SCALE_1_4`, giving 160x120 from a ~7-9 KB frame.

**VGA silently shrinks everything by ~30% — the crop in `inference_service.c`
undoes it.** This is the trap in the fix above, and it is not obvious from the
frame sizes alone. The OV3660 does not *rescale* between `FRAMESIZE_VGA` and
`FRAMESIZE_HD`; the driver writes a **different sensor window** per aspect ratio,
from `ratio_table[]` in `esp32-camera/sensors/private_include/ov3660_settings.h`:

| mode | window | vs full 4:3 |
|---|---|---|
| 4:3 (VGA 640x480) | 2080 x 1548 | full |
| 16:9 (HD 1280x720) | 1952 x 1092 | 93.8% wide, **70.5% tall** |

HD crops ~30% off the top and bottom. Both map onto the same 160x160 model
input, so in VGA an object arrives **29.5% smaller vertically and 6.2% smaller
horizontally** than it did in HD.

That is not cosmetic here. FOMO emits a 20x20 heat map = **8 px per cell** at
160x160, and the documented 10th-percentile feces blob is **~8.8 px** — just
above one cell. At 70.5% scale it becomes **~6.2 px, below one cell**, so the
smallest, most marginal feces are the first to stop registering. A pure
resolution drop would have quietly hobbled the weakest class in the exact
direction nobody was watching.

`prv_resize_rgb` therefore crops the decoded 160x120 to **150x84 at offset
(5,18)** — the 16:9 window, centred — before resampling to 160x160. That
restores the pre-change sensor-window-to-model mapping exactly while keeping the
~7 KB frame and ~720 ms inference. The fractions are computed from the driver's
window numbers rather than hardcoded, so they still hold if the sensor or
framesize changes. Cost: the camera sees the narrower 16:9 field again, i.e.
the same framing as before, not the ~42% taller 4:3 view.

The counter-argument, for the record: `dataset/images` is uniformly **512x512
square** (2468 train + 488 val, all 1:1), and the 4:3→square squash stretches
objects 1.33x vertically versus 1.78x for 16:9 — so VGA is geometrically
*closer* to the training distribution. But that argues about shape, while the
scale loss is a measured number, and scale decides whether a blob survives its
own heat-map cell. Parity was the safer default.

**Live view was capped at 2 fps by a frame-size estimate — 2026-10-13.** Fixed,
and the lesson is that a plausible-looking comment cost real performance:

| | before | after |
|---|---|---|
| `fps` (live view) | 2.0-2.25 | **0.6-15.7**, set by RSSI, not by firmware |
| frame size | 25,753 B @1280x720 | ~7,000-8,900 B @640x480 |
| `inferenceMs` | 1042-1135 | **720-815** |
| `dsp` (JPEG decode) | 13 ms | 10 ms |
| ping loss | 16% | **0%** |
| `STREAM_BUDGET_BPS` | 61440 | 122880 |

Three independent causes, in order of impact:

1. **Frame size vs byte budget.** 61440 / 25753 = 2.4 fps. This was a *design*
   cap, not a link problem — the budget had been sized against a frame size that
   never existed. Resolution is now the frame-size lever; JPEG quality was left
   at 30 so image fidelity per pixel did not change.
2. **CPU pinned at 160 MHz.** Not the S3's maximum, and it bought nothing: the
   SDK's own split (`classify: dsp=13ms infer=1042ms post=0ms`) shows the
   network is the *entire* cost. The tensor arena is **479,948 bytes** and is
   heap-allocated (`EI_CLASSIFIER_ALLOCATION_STATIC` off), so with
   `SPIRAM_USE_MALLOC=y` + `ALWAYSINTERNAL=4096` it lands in PSRAM alongside
   every camera framebuffer — 469 KB touched at every convolution. At 240 MHz
   inference drops to ~720 ms and **ping loss went 16% → 0%**, which suggests
   the 1-second CPU burst was starving the Wi-Fi stack, not just slowing it.
   That makes the old "intermittent ESP_ERR_HTTP_CONNECT" note below at least
   partly a CPU-scheduling artefact.
3. **The 2.4 GHz link is now the binding constraint.** fps tracks RSSI directly
   and swings far more than any firmware knob:
   **-50 dBm → 15.7 fps, -58 → ~12, -63 → 1.9, -79 → 0.6**. The board sits on
   the router's 2.4 GHz radio (the laptop is on 5 GHz, ch 132) and the 2.4 GHz
   signal is genuinely weak here — -74/-79 dBm is at or past the firmware's own
   WEAK threshold of -75. Ping loss is 0% even at that RSSI, so this is signal
   strength, not interference. **Moving the board or the router's 2.4 GHz
   antenna is worth more than any remaining firmware change.** Quoting a single
   fps figure here is misleading: measure it together with `wifiRssi`.

**PSRAM was left at 80 MHz deliberately.** The only step up on the S3 is 120 MHz,
which IDF 5.5.5 marks experimental: it "will crash randomly" after a ~20 °C
temperature swing. Not acceptable on a board with a pump relay next to it.

**`sdkconfig.defaults` is gitignored unless explicitly un-ignored — it now isn't.**
Worth knowing, because this cost a real build: ESP-IDF's own default for the
S3 is **160 MHz** (`esp_system/port/soc/esp32s3/Kconfig.cpu`), so a fresh clone
would have silently built the slow-clock firmware and lost ~1/3 of inference
throughput with nothing in the diff to explain it. The root `.gitignore` had
`sdkconfig*`, which also swallowed `sdkconfig.defaults` — the per-checked-in
project defaults that are supposed to be tracked. There is now a negation for
that one file; the generated `sdkconfig` and `build/` stay ignored.
Related trap: **`sdkconfig.defaults` does NOT override an existing `sdkconfig`.**
Editing the defaults file and rebuilding changed nothing — the stored value
wins. The live `sdkconfig` had to be edited too for 240 MHz to take effect.

**The camera being black, not the resolution change, is why detection looked
dead for most of 2026-10-13.** Sequence worth remembering, because the two
problems were unrelated and interleaved:

- Early evening the lens was unlit/covered. The live frame decoded correctly at
  640x480 but was **completely black**, so `boxes=0` and `class=-1` were the
  *correct* answers, not a regression — and they look identical to a broken
  pipeline, which is why the framing work above could not be validated at the
  time.
- Once the camera was uncovered and aimed at the pen, scores went to 0.28-0.45
  and detection matched the afternoon exactly:

  | window | rows | sprays | avg `yolo_conf` |
  |---|---|---|---|
  | afternoon 17-19h | 20 | 19 | 0.3382 |
  | last hour (23:48+) | 15 | 13 | 0.3482 |

  Same hit rate, marginally higher confidence. The decode path had been healthy
  all along (`dsp=10ms` is real work; a divisor/enum mismatch returns
  `ESP_ERR_INVALID_ARG` instantly and would read ~0 ms).
- **Lesson: `boxes=0` on an unlit camera and `boxes=0` on a broken pipeline are
  the same reading.** Pull a frame and *look* at it before concluding anything.

**These sprays are not yet proven accurate.** At the 0.28 gate, **36% of
pig-only frames fire** (measured: 30 feces-labelled val images vs all 22
pig-only images; positives mean 0.263, negatives mean 0.172, but the
distributions overlap badly). The dataset is **99.3% positive** — only 22
negatives out of 2962 images, because Roboflow exported annotated crops only.
The model has barely seen a "no feces" frame. So a spray is proof the
plumbing works, *not* proof it identified feces. Confirm by watching the pen
against the history log.

**`VERIFY OFFLINE` in the live view is RSSI flapping, not a broken verifier.**
The label comes from one condition in `dashboard/assets/js`:
`VRF_STALE_MS = 6000` and `vrfStale = v.valid && v.ageMs > VRF_STALE_MS`, which
`views/live/index.php` renders as `VERIFY OFFLINE`. `VERIFY_POLL_MS` is 2000, so
6 s allows only ~3 polls of tolerance — a single dip trips it and the 1.5 s
status poll usually catches it mid-outage. Caught a full cycle in 40 s:

```
wifiRssi:-62  verifyOk:false  failures:243  age:50400   <- OFFLINE
wifiRssi:-63  verifyOk:true   failures:0    age:414      <- recovered
wifiRssi:-79  verifyOk:true   failures:0    age:606
```

The verifier was healthy throughout: `/health` 200 in 0.21 s and **12 x
`POST /verify 200 OK` arriving in 40 s** from the board. Only the board's
connects time out, below roughly -67 dBm. **Do not read this label as "detection
is broken"** — detection and the verify overlay are independent.

**Socket count to watch: ~16 ESTABLISHED vs `CONFIG_LWIP_MAX_SOCKETS=10`.**
The laptop holds ~16-17 established connections from the board, each with a
distinct source port, and the count creeps upward (16 → 17 over 25 s). The board
has fewer socket slots than are held open against it, which likely contributes
to the `sock < 0` errors. Not yet investigated — check whether
`esp_http_client_cleanup` is reached on every path out of `prv_poll_verify`,
and whether keep-alive should be disabled client-side. (An earlier note
dismissing these as ordinary uvicorn keep-alive was wrong.)

**`cooldown detected` in the history log is a MISNOMER — do not trust it.**
`detection_service.c` writes `triggered ? "auto_spray" : "cooldown_detected"`, so
*every* non-spray detection is labelled "cooldown" regardless of the real
reason. Rows like:

```
247  2026-10-13 23:36:33  confidence 0.1055  triggered 0  cooldown detected
```

were **not** blocked by cooldown — `cooldownActive` was false throughout. They
were rejected at `spray_controller.c:257` for scoring below the **0.60 device
dial**. Anyone reading that string will chase a cooldown that never engaged.

**The 0.60 device spray dial is unreachable for this model — only the server
gate ever sprays.** Device-only scores on 2026-10-13 averaged **0.165, max
0.285**; the dial is 0.60, so `spray_controller_on_detection` rejects them
outright and the on-device path can never fire. The only rows ever above 0.60
are from Sept 24-30, plus ids 1-4 which are literally labelled `simulated
spray`. The reason is `spray_controller_trigger_external` (`spray_controller.c:237`):
it applies the *caller's* gate rather than the FOMO dial, precisely because the
two models are on different scales. So:

- device FOMO path: dead (0.60 dial vs ~0.17 scores)
- server YOLOv8 path: working, fires at `server_trigger_conf` 0.28

**If the laptop verifier dies, there is no fallback** — the board will keep
detecting and logging but never spray. Measured server-leg performance at 0.28:
63% of feces images caught, **36% false sprays on pig-only frames**.
`esp-tls: select() timeout` → `HTTP_CLIENT: Connection failed, sock < 0` on
`/verify` while the server simultaneously logs `POST /verify 200 OK`. A TCP
*connect* failure, not HTTP or model. RSSI drifts -62…-79 dBm (the firmware's
own WEAK threshold is -75) and ping ranged 17 ms→762 ms, so connects get
starved. Expect missed verifier samples and `yolo_conf=NULL` rows.
2026-10-13: two contributing causes — Wi-Fi starvation from the 160 MHz CPU
burst (fixed by 240 MHz, see cause 2 above) and the weak 2.4 GHz link, which
still trips it below ~-67 dBm.

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