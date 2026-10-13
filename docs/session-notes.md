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
| ESP32-S3 | `192.168.1.39` | **static**, in NVS (see *Network migration* below) |
| Laptop (XAMPP + verifier) | `192.168.1.6` | **DHCP — reserved at the router, but expect this to change** |
| Wi-Fi network | `Meliora` | Windows profile is **Public** |
| Verifier | port `8000` | `server/yolo_verify` |
| Dashboard | port `80` | `/feces-detector-ai/dashboard/` |

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

### The firewall re-scope needs an elevated shell

The `YOLO Verify 8000` rule was still scoped to `192.168.100.0/24`. From a
**non-elevated** shell `Set-NetFirewallRule` fails with `Access is denied.
(System Error 5)` and silently changes nothing — the `Get-` half of the command
still prints the old scope, so it looks like it may have worked. Run it from an
elevated PowerShell:

```powershell
Set-NetFirewallRule -DisplayName "YOLO Verify 8000" -Action Allow
Get-NetFirewallRule -DisplayName "YOLO Verify 8000" |
  Get-NetFirewallAddressFilter |
  Set-NetFirewallAddressFilter -RemoteAddress 192.168.1.0/24
```

It turned out not to matter: `C:\xampp\apache\bin\httpd.exe` and the Python
3.12 interpreter each already carry Public-profile inbound allows on any port,
so the verifier leg worked regardless. Confirmed empirically —
`verifyOkAgeMs` went from `UINT64_MAX` to a real number with
`verifyFailures:0`. Still worth re-scoping rather than leaving a rule that
matches nothing.

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

All runtime config lives in NVS (flash partition `0x9000`, `partitions.csv:2`),
written by `app_config_save()` in `app_config.c`. Power-cycling loses nothing —
the board comes back on `192.168.100.39` still pointing at `192.168.100.227`.
Only `idf.py erase_flash` wipes it, and then the SSID/password must be
re-entered over USB per `docs/setup-guide.md` §7.5.

**Careful:** rewriting the NVS partition over USB *does* reset every key not in
the CSV — threshold, cooldown, spray timing, relay polarity and the URLs all
fall back to the compiled defaults. Writing the whole set by hand avoids the
round-2 `POST /config`, but it also means the values are whatever you typed, not
whatever the board had been running. See *Network migration* above.

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
connections. A rule named `YOLO Verify 8000` exists in Windows and is scoped to
a subnet. **Re-scope it on every network change** — this has now bitten on both
the 2026-10-12 and the 2026-10-13 moves:

```powershell
Set-NetFirewallRule -DisplayName "YOLO Verify 8000" -Action Allow
Get-NetFirewallRule -DisplayName "YOLO Verify 8000" |
  Get-NetFirewallAddressFilter |
  Set-NetFirewallAddressFilter -RemoteAddress 192.168.1.0/24
```

Needs an **elevated** shell, and check it took: from a normal shell `Set-`
fails with `Access is denied. (System Error 5)` while the trailing `Get-`
still prints the old scope, which reads like a partial success. Both
`C:\xampp\apache\bin\httpd.exe` and the Python interpreter already carry
Public-profile inbound allows on any port, so this rule is belt-and-braces
rather than the only thing standing between the board and the verifier.

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
outbound leg is broken — check the verifier is up, the firewall rule matches
the *current* subnet, and the laptop's IP is still `192.168.1.6`.

## Open issues

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