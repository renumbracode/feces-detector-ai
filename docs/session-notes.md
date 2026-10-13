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
| ESP32-S3 | `192.168.100.39` | **static**, in NVS (see *Network migration* below) |
| Laptop (XAMPP + verifier) | `192.168.100.227` | **DHCP — expect this to change** |
| Wi-Fi network | `ALTHEA 11` | Windows profile is **Public** |
| Verifier | port `8000` | `server/yolo_verify` |
| Dashboard | port `80` | `/feces-detector-ai/dashboard/` |

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
connections. A rule named `YOLO Verify 8000` exists in Windows but is still
scoped to `192.168.1.0/24`, which no longer matches anything. **Re-scope it on
every network change** — this bit again on 2026-10-12:

```powershell
Set-NetFirewallRule -DisplayName "YOLO Verify 8000" -Action Allow
Get-NetFirewallRule -DisplayName "YOLO Verify 8000" |
  Get-NetFirewallAddressFilter |
  Set-NetFirewallAddressFilter -RemoteAddress 192.168.100.0/24
```

Needs an elevated shell. Both `Apache HTTP Server` and `python.exe` already
carry Public-profile inbound allows on any port, so this rule is belt-and-braces
rather than the only thing standing between the board and the verifier.

## Confirming the whole path

```bash
curl -s http://192.168.100.39/status
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
the *current* subnet, and the laptop's IP is still `192.168.100.227`.

## Open issues

**The camera was aimed at a wall for several hours.** No bug in the pipeline —
`lastConfidence:0.0000` and `detections:0` were the correct answer to a photo
of a blank surface. The model reports `pig 0.408` / `feces 0.078` on an indoor
desk scene; those are texture false positives, not detections. Always pull a
frame and *look at it* before believing the detector is broken:

```bash
curl -s --max-time 12 -o s.bin http://192.168.100.39/stream
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